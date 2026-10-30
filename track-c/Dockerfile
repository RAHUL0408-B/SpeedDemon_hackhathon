FROM gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95
WORKDIR /app
COPY solution.cpp .
RUN g++ -std=c++23 -O3 -DNDEBUG -march=native -flto -fno-exceptions -fno-rtti -pthread solution.cpp -o solution
ENTRYPOINT ["/app/solution"]
