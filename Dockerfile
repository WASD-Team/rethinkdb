
FROM debian:jessie-slim as deps

RUN apt-get update
RUN apt-get install -y mg &&\
    apt-get install -y build-essential protobuf-compiler python \
        libprotobuf-dev libcurl4-openssl-dev libboost-all-dev \
        libncurses5-dev libjemalloc-dev wget m4 clang libssl-dev

FROM deps as build

WORKDIR /root
COPY . .

RUN ./configure --allow-fetch CXX=clang++
RUN make -j8

FROM deps as run

COPY --from=build /root/build/release_clang/rethinkdb /usr/bin/rethinkdb

VOLUME ["/data"]

WORKDIR /data

CMD ["rethinkdb", "--bind", "all"]

#   process cluster webui
EXPOSE 28015 29015 8080
