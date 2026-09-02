class Structure {
    int Run(int value) {
        System.Func<int, int> adjust = step => {
            foreach (int item in new[] { 1, 2 }) {
                if (item > 0) {
                    value += item switch {
                        1 => step,
                        _ => 0,
                    };
                }
            }
            return value;
        };
        try {
            while (value < 4) value++;
        } catch (System.Exception) {
            return 0;
        } finally {
            value++;
        }
        return adjust(1);
    }
}
