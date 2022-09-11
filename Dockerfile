FROM ubuntu:20.04 as builder_base

COPY scripts/install-builder-deps.sh .
RUN bash ./install-builder-deps.sh

FROM builder_base as prime_server_builder

COPY scripts/install-primeserver-deps.sh .
RUN bash ./install-primeserver-deps.sh

WORKDIR /prime_server/prime_server
RUN ./autogen.sh && ./configure LDFLAGS="-fPIC" CPPFLAGS="-fPIC" --prefix /prime_server && make -j$(nproc) V=1 && make install

FROM builder_base as builder

# set paths
ENV PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH
ENV LD_LIBRARY_PATH /usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib32:/usr/lib32
ENV LIBRARY_PATH /usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/usr/lib/aarch64-linux-gnu:/lib32:/usr/lib32

# get the code into the right place and prepare to build it
WORKDIR /usr/local/src/valhalla
COPY .git /usr/local/src/valhalla/.git
RUN git submodule sync && git submodule update --init --recursive
RUN mkdir build

# copy prime_server artifacts
COPY --from=prime_server_builder /usr/local /usr/local
COPY --from=prime_server_builder /prime_server/bin/prime_* /usr/bin/
COPY --from=prime_server_builder /prime_server/lib/libprime* /usr/lib/*-linux-gnu/
COPY --from=prime_server_builder /prime_server/include/prime_server/* /usr/include/prime_server/

# configure the build with symbols turned on so that crashes can be triaged
WORKDIR /usr/local/src/valhalla/build

COPY --from=prime_server_builder /prime_server/lib/libprime* /usr/local/src/valhalla/build/prime_server_lib/

COPY . /usr/local/src/valhalla/

# RUN ls -l /usr/local/src/valhalla/build/prime_server_lib/ && ls -l /usr/local/src/valhalla/ && git status && exit 1

RUN cmake .. -LAH -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_PRIME_SERVER_VERSION_CHECK=NO \
    -DPRIME_SERVER_INCLUDE_DIR="/usr/include/" \
    -DPRIME_SERVER_LIB="/usr/local/src/valhalla/build/prime_server_lib/libprime_server.a" \
    -DCZMQ_LIB="/usr/local/libczmq.a" \
    -DENABLE_WERROR=OFF \
    -DCMAKE_C_COMPILER=gcc
RUN make all -j$(nproc) VERBOSE=1
RUN make test
RUN make install

# we wont leave the source around but we'll drop the commit hash we'll also keep the locales
WORKDIR /usr/local/src
RUN cd valhalla && echo "https://github.com/valhalla/valhalla/tree/$(git rev-parse HEAD)" > ../valhalla_version
RUN for f in valhalla/locales/*.json; do cat ${f} | python3 -c 'import sys; import json; print(json.load(sys.stdin)["posix_locale"])'; done > valhalla_locales
RUN rm -rf valhalla

# the binaries are huge with all the symbols so we strip them but keep the debug there if we need it
WORKDIR /usr/local/bin
RUN for f in valhalla_*; do objcopy --only-keep-debug $f $f.debug; done
RUN tar -cvf valhalla.debug.tar valhalla_*.debug && gzip -1 valhalla.debug.tar
RUN rm -f valhalla_*.debug
RUN strip --strip-debug --strip-unneeded valhalla_* || true
RUN strip /usr/local/lib/libvalhalla.a
RUN strip /usr/lib/python3/dist-packages/valhalla/python_valhalla.cpython-38-*-linux-gnu.so

####################################################################
# copy the important stuff from the build stage to the runner image
FROM ubuntu:20.04 as runner

COPY --from=prime_server_builder /usr/local /usr/local
COPY --from=builder /usr/local /usr/local
COPY --from=prime_server_builder /prime_server/bin/prime_* /usr/bin/
COPY --from=prime_server_builder /prime_server/lib/libprime* /usr/lib/*-linux-gnu/
COPY --from=builder /usr/lib/python3/dist-packages/valhalla/* /usr/lib/python3/dist-packages/valhalla/

# we need to add back some runtime dependencies for binaries and scripts
# install all the posix locales that we support
RUN export DEBIAN_FRONTEND=noninteractive && apt update && \
    apt install -y \
      libcurl4 libczmq4 libluajit-5.1-2 \
      libprotobuf-lite17 libsqlite3-0 libsqlite3-mod-spatialite libzmq5 zlib1g \
      curl gdb locales parallel python3.8-minimal python3-distutils python-is-python3 \
      spatialite-bin unzip wget && \
    cat /usr/local/src/valhalla_locales | xargs -d '\n' -n1 locale-gen && \
    rm -rf /var/lib/apt/lists/* && \
    \
    # python smoke test
    python3 -c "import valhalla,sys; print (sys.version, valhalla)"
