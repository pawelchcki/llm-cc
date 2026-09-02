#!/usr/bin/env node
const text = "// not a comment /* either */";
const template = `/* still text */ // ${"still text"}`;
const regex = /\/\* not a comment \*\//;
const value = 1; // line comment
/* block comment
   second line */
<!-- html comment
const next = 2;
