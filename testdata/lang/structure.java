class Structure {
    int run(int value) {
        var adjust = (java.util.function.IntUnaryOperator) step -> {
            for (int item : new int[] {1, 2}) {
                if (item > 0) {
                    value += switch (item) {
                        case 1 -> step;
                        default -> 0;
                    };
                }
            }
            return value;
        };
        try (var input = new java.io.StringReader("x")) {
            while (input.read() >= 0) {
                value++;
            }
        } catch (java.io.IOException error) {
            return 0;
        } finally {
            value++;
        }
        return adjust.applyAsInt(1);
    }
}
