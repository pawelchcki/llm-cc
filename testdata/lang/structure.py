class Structure:
    def run(self, value):
        adjust = lambda step: value + step
        for item in (1, 2):
            if item > 0:
                match item:
                    case 1:
                        value += 1
                    case _:
                        value += 0
        try:
            while value < 4:
                value += 1
        except ValueError:
            return 0
        finally:
            value += 1
        return adjust(1)
