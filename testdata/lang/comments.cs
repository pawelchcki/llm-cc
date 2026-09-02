class Comments {
    string text = "// not a comment /* either */";
    string verbatim = @"/* still text */ // still text";
    string raw = """/* still text */ // still text""";
    int value = 1; // line comment
    /* block comment
       second line */
    int next = 2;
}
