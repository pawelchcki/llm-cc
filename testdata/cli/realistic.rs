struct Counter {
    total: i32,
}

impl Counter {
    fn add(&mut self, value: i32) {
        if value > 0 {
            self.total += value;
        }
    }
}

fn main() {
    let mut counter = Counter { total: 0 };
    counter.add(3);
}
