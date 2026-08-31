#define STRINGIZE(value) #value
// macro-adjacent comment

const char *url = "https://example.test/* literal */";

int sample(void) {
    /* block comment
       on two lines */
    int joined = left/* join safely */right;
    const char *name = STRINGIZE(not_a_comment);
    return joined; // trailing comment
}
