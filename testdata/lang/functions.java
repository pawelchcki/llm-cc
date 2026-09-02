interface Contract {
    void declaration();
}

record Point(int x) {
    Point {
        if (x < 0) throw new IllegalArgumentException();
    }
}

class Widget {
    Widget() {}

    int compute() {
        Runnable nested = () -> {
            class Local {
                void hidden() {}
            }
        };
        return 1;
    }
}
