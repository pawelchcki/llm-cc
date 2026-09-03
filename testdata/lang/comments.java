class Comments {
    String text = "// not a comment /* either */";
    String block = """
        /* still text */ // still text
        """;
    int value = 1; // line comment
    /* block comment
       second line */
    int next = 2;
}
