namespace Ns {
class Cls {
 public:
  ~Cls();
};
}  // namespace Ns

int* pointer_result() { return nullptr; }

int& reference_result(int& value) { return value; }

Ns::Cls::~Cls() = default;

struct Stream {};
Stream& operator<<(Stream& stream, int) { return stream; }

struct Convertible {
  explicit operator bool() const { return true; }
};
