struct Worker;

impl Worker {
    fn method(&self) {}
}

fn outer() {
    fn nested() {}
    nested();
}
