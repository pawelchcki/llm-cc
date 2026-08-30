namespace sample {
class Runner {
public:
    int run(int value) {
        auto adjust = [value](int step) {
            for (int item : {1, 2}) {
                if (item > 0) {
                    switch (item) {
                        case 1: value += step; break;
                        default: break;
                    }
                }
            }
            return value;
        };
        try {
            return adjust(1);
        } catch (...) {
            return 0;
        }
    }
};

enum class State { ready, done };
}
