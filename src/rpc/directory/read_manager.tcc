// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef RPC_DIRECTORY_READ_MANAGER_TCC_
#define RPC_DIRECTORY_READ_MANAGER_TCC_

#include "rpc/directory/read_manager.hpp"

#include <map>
#include <utility>

#include "concurrency/wait_any.hpp"
#include "containers/archive/archive.hpp"

template<class metadata_t>
directory_read_manager_t<metadata_t>::directory_read_manager_t(connectivity_service_t *conn_serv) THROWS_NOTHING :
    connectivity_service(conn_serv),
    variable(change_tracking_map_t<peer_id_t, metadata_t>()),
    connectivity_subscription(this) {
    connectivity_service_t::peers_list_freeze_t freeze(connectivity_service);
    guarantee(connectivity_service->get_peers_list().empty());
    connectivity_subscription.reset(connectivity_service, &freeze);
}

template<class metadata_t>
directory_read_manager_t<metadata_t>::~directory_read_manager_t() THROWS_NOTHING {
    guarantee(connectivity_service->get_peers_list().empty());
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::on_connect(peer_id_t peer) THROWS_NOTHING {
    assert_thread();
    std::pair<typename boost::ptr_map<peer_id_t, session_t>::iterator, bool> res = sessions.insert(peer, new session_t(connectivity_service->get_connection_session_id(peer)));
    guarantee(res.second);
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::on_message(peer_id_t source_peer, string_read_stream_t *s) THROWS_NOTHING {
    uint8_t code = 0;
    {
        archive_result_t res = deserialize(s, &code);
        guarantee_deserialization(res, "code");
    }

    switch (code) {
        case 'I': {
            /* Initial message from another peer */
            boost::shared_ptr<metadata_t> initial_value(new metadata_t());
            fifo_enforcer_state_t metadata_fifo_state;
            {
                archive_result_t res = deserialize(s, initial_value.get());
                guarantee_deserialization(res, "metadata");
                res = deserialize(s, &metadata_fifo_state);
                guarantee_deserialization(res, "metadata fifo state");
                guarantee(!res);
            }

            /* Spawn a new coroutine because we might not be on the home thread
            and `on_message()` isn't supposed to block very long */
            coro_t::spawn_sometime(boost::bind(
                &directory_read_manager_t::propagate_initialization, this,
                source_peer, connectivity_service->get_connection_session_id(source_peer),
                initial_value, metadata_fifo_state,
                auto_drainer_t::lock_t(per_thread_drainers.get())));

            break;
        }

        case 'U': {
            /* Update from another peer */
            boost::shared_ptr<metadata_t> new_value(new metadata_t());
            fifo_enforcer_write_token_t metadata_fifo_token;
            {
                archive_result_t res = deserialize(s, new_value.get());
                guarantee_deserialization(res, "metadata");
                res = deserialize(s, &metadata_fifo_token);
                guarantee_deserialization(res, "metadata fifo state");

                // TODO Don't fail catastrophically just because there's bad data on the stream.
            }

            /* Spawn a new coroutine because we might not be on the home thread
            and `on_message()` isn't supposed to block very long */
            coro_t::spawn_sometime(boost::bind(
                &directory_read_manager_t::propagate_update, this,
                source_peer, connectivity_service->get_connection_session_id(source_peer),
                new_value, metadata_fifo_token,
                auto_drainer_t::lock_t(per_thread_drainers.get())));

            break;
        }

        default: unreachable();
    }
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::on_disconnect(peer_id_t peer) THROWS_NOTHING {
    ASSERT_FINITE_CORO_WAITING;
    assert_thread();

    /* Remove the `global_peer_info_t` object from the table */
    typename boost::ptr_map<peer_id_t, session_t>::iterator it = sessions.find(peer);
    guarantee(it != sessions.end());
    session_t *session_to_destroy = sessions.release(it).release();

    bool got_initialization = session_to_destroy->got_initial_message.is_pulsed();

    /* Start interrupting any running calls to `propagate_update()`. We need to
    explicitly interrupt them rather than letting them finish on their own
    because, if the network reordered messages, they might wait indefinitely for
    a message that will now never come. */
    coro_t::spawn_sometime(boost::bind(
        &directory_read_manager_t::interrupt_updates_and_free_session, this,
        session_to_destroy,
        auto_drainer_t::lock_t(&global_drainer)));

    /* Notify that the peer has disconnected */
    if (got_initialization) {
        DEBUG_VAR mutex_assertion_t::acq_t acq(&variable_lock);

        struct op_closure_t {
            bool apply(change_tracking_map_t<peer_id_t, metadata_t> *map) {
                map->begin_version();
                map->delete_value(*peer);
                return true;
            }
            explicit op_closure_t(const peer_id_t *c1) :
                peer(c1) { }
            const peer_id_t *peer;
        };
        op_closure_t op(&peer);

        variable.apply_atomic_op(std::bind(&op_closure_t::apply, &op,
                                           std::placeholders::_1));
    }
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::propagate_initialization(peer_id_t peer, uuid_u session_id, const boost::shared_ptr<metadata_t> &initial_value, fifo_enforcer_state_t metadata_fifo_state, auto_drainer_t::lock_t per_thread_keepalive) THROWS_NOTHING {
    per_thread_keepalive.assert_is_holding(per_thread_drainers.get());
    on_thread_t thread_switcher(home_thread());

    ASSERT_FINITE_CORO_WAITING;
    /* Check to make sure that the peer didn't die while we were coming from the
    thread on which `on_message()` was run */
    typename boost::ptr_map<peer_id_t, session_t>::iterator it = sessions.find(peer);
    if (it == sessions.end()) {
        /* The peer disconnected since we got the message; ignore. */
        return;
    }
    session_t *session = it->second;
    if (session->session_id != session_id) {
        /* The peer disconnected and then reconnected since we got the message;
        ignore. */
        return;
    }

    /* Notify that the peer has connected */
    {
        DEBUG_VAR mutex_assertion_t::acq_t acq(&variable_lock);

        struct op_closure_t {
            bool apply(change_tracking_map_t<peer_id_t, metadata_t> *map) {
                map->begin_version();
                map->set_value(*peer, std::move(*initial_value));
                return true;
            }
            op_closure_t(const peer_id_t *c1,
                         const boost::shared_ptr<metadata_t> &c2) :
                peer(c1),
                initial_value(c2) { }
            const peer_id_t *peer;
            boost::shared_ptr<metadata_t> initial_value;
        };
        op_closure_t op(&peer, initial_value);

        variable.apply_atomic_op(std::bind(&op_closure_t::apply, &op,
                                           std::placeholders::_1));
    }

    /* Create a metadata FIFO sink and pulse the `got_initial_message` cond so
    that instances of `propagate_update()` can proceed */
    // TODO: Do we want this .reset() here?  It is not easily provable
    // that it'll be initialized only once.
    session->metadata_fifo_sink.reset();
    session->metadata_fifo_sink.init(new fifo_enforcer_sink_t(metadata_fifo_state));
    session->got_initial_message.pulse();
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::propagate_update(peer_id_t peer, uuid_u session_id, const boost::shared_ptr<metadata_t> &new_value, fifo_enforcer_write_token_t metadata_fifo_token, auto_drainer_t::lock_t per_thread_keepalive) THROWS_NOTHING {
    per_thread_keepalive.assert_is_holding(per_thread_drainers.get());
    on_thread_t thread_switcher(home_thread());

    /* Check to make sure that the peer didn't die while we were coming from the
    thread on which `on_message()` was run */
    typename boost::ptr_map<peer_id_t, session_t>::iterator it = sessions.find(peer);
    if (it == sessions.end()) {
        /* The peer disconnected since we got the message; ignore. */
        return;
    }
    session_t *session = it->second;
    if (session->session_id != session_id) {
        /* The peer disconnected and then reconnected since we got the message;
        ignore. */
        return;
    }
    auto_drainer_t::lock_t session_keepalive(&session->drainer);

    try {
        /* Wait until we got an initialization message from this peer */
        {
            wait_any_t waiter(&session->got_initial_message, session_keepalive.get_drain_signal());
            waiter.wait_lazily_unordered();
            if (session_keepalive.get_drain_signal()->is_pulsed()) {
                throw interrupted_exc_t();
            }
        }

        /* Exit this peer's `metadata_fifo_sink` so that we perform the updates
        in the same order as they were performed at the source. */
        fifo_enforcer_sink_t::exit_write_t fifo_exit(session->metadata_fifo_sink.get(),
                                                     metadata_fifo_token);
        wait_interruptible(&fifo_exit, session_keepalive.get_drain_signal());

        {
            DEBUG_VAR mutex_assertion_t::acq_t acq(&variable_lock);

            struct op_closure_t {
                bool apply(change_tracking_map_t<peer_id_t, metadata_t> *map) {
                    typename std::map<peer_id_t, metadata_t>::const_iterator var_it
                        = map->get_inner().find(*peer);
                    if (var_it == map->get_inner().end()) {
                        guarantee(!std_contains(*sessions, *peer));
                        //The session was deleted we can ignore this update.
                        return false;
                    }
                    map->begin_version();
                    map->set_value(*peer, std::move(*new_value));
                    return true;
                }
                op_closure_t(const peer_id_t *c1,
                             const boost::shared_ptr<metadata_t> &c2,
                             const boost::ptr_map<peer_id_t, session_t> *c3) :
                    peer(c1),
                    new_value(c2),
                    sessions(c3) { }
                const peer_id_t *peer;
                const boost::shared_ptr<metadata_t> new_value;
                const boost::ptr_map<peer_id_t, session_t> *sessions;
            };
            op_closure_t op(&peer, new_value, &sessions);

            variable.apply_atomic_op(std::bind(&op_closure_t::apply, &op,
                                               std::placeholders::_1));
        }
    } catch (const interrupted_exc_t &) {
        /* Here's what happened: `on_disconnect()` was called for the peer. It
        spawned `interrupt_updates_and_free_session()`, which deleted the
        `session_t` for this peer, thereby calling the destructor for the peer's
        `auto_drainer_t`, for which `session_keepalive` is an
        `auto_drainer_t::lock_t`. `session_keepalive.get_drain_signal()` was
        pulsed, so an `interrupted_exc_t` was thrown.

        The peer has disconnected, so we can safely ignore this update; its
        entry in the thread-local peer table will be deleted soon anyway. */
    }
}

template<class metadata_t>
void directory_read_manager_t<metadata_t>::interrupt_updates_and_free_session(session_t *session, auto_drainer_t::lock_t global_keepalive) THROWS_NOTHING {
    assert_thread();
    global_keepalive.assert_is_holding(&global_drainer);

    /* This must be done in a separate coroutine because `session_t` has an
    `auto_drainer_t`, and the `auto_drainer_t` destructor can block. */
    delete session;
}

#endif  // RPC_DIRECTORY_READ_MANAGER_TCC_
