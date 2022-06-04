FROM emscripten/emsdk:3.1.4 as zlib_builder

WORKDIR /build/

RUN curl -L -o zlib.tar.gz https://www.zlib.net/zlib-1.2.12.tar.gz

RUN bash -c 'echo "91844808532e5ce316b3c010929493c0244f3d37593afd6de04f71821d5136d9  zlib.tar.gz" | sha256sum --check'

RUN tar -xvf zlib.tar.gz

WORKDIR /build/zlib-1.2.12

RUN emconfigure ./configure

RUN emmake make -j4

FROM emscripten/emsdk:3.1.4 as protobuf_builder

RUN apt-get update -y
RUN apt-get install -y build-essential

WORKDIR /build/

RUN git clone https://github.com/google/protobuf && cd protobuf && git checkout 6a59a2ad1f61d9696092f79b6d74368b4d7970a3

RUN git clone https://github.com/kwonoj/protobuf-wasm && cd protobuf-wasm && git checkout 4bba8b2f38b5004f87489642b6ca4525ae72fe7f

RUN cp ./protobuf-wasm/*.patch ./protobuf

RUN cd protobuf && git apply *.patch

WORKDIR /build/protobuf

RUN apt-get install -y autoconf libtool-bin

RUN bash ./autogen.sh

RUN emconfigure ./configure

RUN emmake make -j4

FROM debian:buster-slim as protoc_builder

RUN apt-get update -y
RUN apt-get install -y build-essential autoconf libtool-bin git

WORKDIR /build/

RUN git clone https://github.com/google/protobuf && cd protobuf && git checkout 6a59a2ad1f61d9696092f79b6d74368b4d7970a3

WORKDIR /build/protobuf

RUN bash ./autogen.sh

RUN ./configure

RUN make -j4

FROM emscripten/emsdk:3.1.4 as valhalla_builder

WORKDIR /build

COPY --from=zlib_builder /build/zlib-1.2.12 /build/zlib/
COPY --from=protoc_builder /build/protobuf /build/protobuf/

COPY valhalla /build/valhalla

WORKDIR /build/valhalla/proto

RUN /build/protobuf/src/protoc --cpp_out=. *.proto

WORKDIR /build/valhalla/third_party/OSM-binary/src
RUN /build/protobuf/src/protoc --cpp_out=. *.proto

RUN rm -rf /build/protobuf

COPY --from=protobuf_builder /build/protobuf /build/protobuf/

ENV LIBRARY_PATH=$LIBRARY_PATH:/build/protobuf:build/protobuf/src:/build/zlib
ENV LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/build/protobuf:build/protobuf/src:/build/zlib

WORKDIR /build/valhalla/build

RUN emcmake cmake .. \
  -DCMAKE_FIND_DEBUG_MODE=ON \
  -DENABLE_DATA_TOOLS=off \
  -DENABLE_PYTHON_BINDINGS=off \
  -DENABLE_SERVICES=off \
  -DENABLE_BENCHMARKS=off \
  -DENABLE_HTTP=OFF \
  -DZLIB_LIBRARY=/build/zlib \
  -DZLIB_INCLUDE_DIR=/build/zlib/include \
  -DBoost_NO_SYSTEM_PATHS=ON \
  -DBOOSTROOT=/build/boost_1_71_0/boost \
  -DProtobuf_DIR=/build/protobuf \
  -DProtobuf_ROOT=/build/protobuf

RUN emmake make VERBOSE=1 -j8
