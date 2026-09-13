int main() {
    return static_assert(1), static_assert(0, "this should fail"), 0;
}
