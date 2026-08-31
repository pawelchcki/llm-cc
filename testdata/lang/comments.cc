#define STRINGIZE(value) #value
// macro-adjacent comment

const char *url = "https://example.test/* literal */";
const char *raw = R"tag(// raw /* text */)tag";

int sample() {
    /* block comment
       on two lines */
    int joined = left/* join safely */right;
    auto name = STRINGIZE(not_a_comment);
    return joined; // trailing comment
}
