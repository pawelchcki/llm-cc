int structures(int *values, int count) {
    while (count > 0) {
        for (int index = 0; index < count; ++index) {
            if (values[index] > 0) {
                switch (values[index]) {
                    case 1: values[index]++; break;
                    default: break;
                }
            }
        }
        do { --count; } while (count > 4);
    }
    return count;
}
