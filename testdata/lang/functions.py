@first
@second(value=1)
def decorated(value):
    def hidden():
        return 0
    return value

class Widget:
    @property
    def method(self):
        return 1
