class Number {
    public Number() {}
    ~Number() {}
    public static Number operator +(Number left, Number right) => left;
    public static implicit operator int(Number value) => 0;

    public void Run() {
        void Hidden() {}
        System.Action hidden = () => {};
    }
}
