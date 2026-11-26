FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive VCPKG_ROOT=/opt/vcpkg VCPKG_MAX_CONCURRENCY=2
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config python3 ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
WORKDIR /src
# Build context is the repository root (see backend/render.yaml).
COPY . .
RUN cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=x64-linux-dynamic -DMYGIT_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target mygit --parallel 2 \
    && cmake --install build --prefix /opt/mygit

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libgcc-s1 libssl3t64 libgomp1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home mygit
COPY --from=build /opt/mygit /opt/mygit
COPY --from=build /src/vcpkg_installed/x64-linux-dynamic/lib /opt/mygit/lib
ENV PATH=/opt/mygit/bin:$PATH LD_LIBRARY_PATH=/opt/mygit/lib MYGIT_SERVER_MODE=hosted MYGIT_INFERENCE_BACKEND=stub
USER mygit
EXPOSE 10000
CMD ["mygit", "serve"]
