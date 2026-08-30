namespace demo {
int twice(int value) {
    // ignored by preprocessing
    auto apply = [value](int factor) { return value * factor; };
    return apply(2);
}
}
