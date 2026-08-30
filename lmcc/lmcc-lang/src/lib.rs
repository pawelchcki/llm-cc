//! Language frontends for LM-CC.

pub use lmcc_core::StructuralEvent;
use tree_sitter::{Language, Node, Parser};

const RUST_COMMENT_KINDS: &[&str] = &["line_comment", "block_comment"];
const RUST_STRUCTURAL_KINDS: &[&str] = &[
    "function_item",
    "loop_expression",
    "while_expression",
    "for_expression",
    "if_expression",
    "match_expression",
    "match_block",
    "block",
    "declaration_list",
    "field_declaration_list",
    "ordered_field_declaration_list",
    "enum_variant_list",
];
const C_COMMENT_KINDS: &[&str] = &["comment"];
const C_STRUCTURAL_KINDS: &[&str] = &[
    "function_definition",
    "compound_statement",
    "for_statement",
    "while_statement",
    "do_statement",
    "if_statement",
    "switch_statement",
];
const CPP_STRUCTURAL_KINDS: &[&str] = &[
    "function_definition",
    "compound_statement",
    "for_statement",
    "while_statement",
    "do_statement",
    "if_statement",
    "switch_statement",
    "for_range_loop",
    "lambda_expression",
    "try_statement",
    "namespace_definition",
    "field_declaration_list",
    "enumerator_list",
];

/// Maps every preprocessed byte boundary to a byte boundary in the original
/// source. Its length is always `preprocessed.len() + 1`.
pub type OffsetMap = Vec<usize>;

pub trait LanguageFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap);
    fn structural_events(&self, src: &str) -> Vec<StructuralEvent>;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct RustFrontend;

#[derive(Debug, Default, Clone, Copy)]
pub struct CFrontend;

#[derive(Debug, Default, Clone, Copy)]
pub struct CppFrontend;

impl LanguageFrontend for RustFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap) {
        strip_comments(src, tree_sitter_rust::LANGUAGE.into(), RUST_COMMENT_KINDS)
    }

    fn structural_events(&self, src: &str) -> Vec<StructuralEvent> {
        structural_events(
            src,
            tree_sitter_rust::LANGUAGE.into(),
            RUST_STRUCTURAL_KINDS,
        )
    }
}

impl LanguageFrontend for CFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap) {
        strip_comments(src, tree_sitter_c::LANGUAGE.into(), C_COMMENT_KINDS)
    }

    fn structural_events(&self, src: &str) -> Vec<StructuralEvent> {
        structural_events(src, tree_sitter_c::LANGUAGE.into(), C_STRUCTURAL_KINDS)
    }
}

impl LanguageFrontend for CppFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap) {
        strip_comments(src, tree_sitter_cpp::LANGUAGE.into(), C_COMMENT_KINDS)
    }

    fn structural_events(&self, src: &str) -> Vec<StructuralEvent> {
        structural_events(src, tree_sitter_cpp::LANGUAGE.into(), CPP_STRUCTURAL_KINDS)
    }
}

fn parser(language: Language) -> Parser {
    let mut parser = Parser::new();
    parser
        .set_language(&language)
        .expect("tree-sitter language must load");
    parser
}

fn strip_comments(src: &str, language: Language, comment_kinds: &[&str]) -> (String, OffsetMap) {
    let mut parser = parser(language);
    let tree = parser
        .parse(src, None)
        .expect("tree-sitter parser returned no tree");
    let mut comment_ranges = Vec::new();
    collect_comment_ranges(tree.root_node(), comment_kinds, &mut comment_ranges);
    comment_ranges.sort_unstable();

    let source = src.as_bytes();
    let mut output = Vec::with_capacity(source.len());
    let mut offset_map = Vec::with_capacity(source.len() + 1);
    let mut cursor = 0;
    for (start, end) in comment_ranges {
        if start < cursor {
            continue;
        }
        append_source(source, cursor, start, &mut output, &mut offset_map);

        let mut preserved_newline = false;
        for (index, byte) in source.iter().enumerate().take(end).skip(start) {
            if *byte == b'\n' {
                output.push(b'\n');
                offset_map.push(index);
                preserved_newline = true;
            }
        }
        if !preserved_newline
            && output
                .last()
                .is_some_and(|byte| !byte.is_ascii_whitespace())
            && source
                .get(end)
                .is_some_and(|byte| !byte.is_ascii_whitespace())
        {
            output.push(b' ');
            offset_map.push(start);
        }
        cursor = end;
    }
    append_source(source, cursor, source.len(), &mut output, &mut offset_map);
    offset_map.push(source.len());

    (
        String::from_utf8(output).expect("removing UTF-8 ranges preserves UTF-8"),
        offset_map,
    )
}

fn structural_events(
    src: &str,
    language: Language,
    structural_kinds: &[&str],
) -> Vec<StructuralEvent> {
    let mut parser = parser(language);
    let tree = parser
        .parse(src, None)
        .expect("tree-sitter parser returned no tree");
    let mut events = Vec::new();
    collect_structural_events(tree.root_node(), 0, structural_kinds, &mut events);
    events.sort_unstable_by_key(|event| (event.byte_offset, event.depth, event.scope_start));
    events.dedup();
    events
}

fn append_source(
    source: &[u8],
    start: usize,
    end: usize,
    output: &mut Vec<u8>,
    offset_map: &mut OffsetMap,
) {
    output.extend_from_slice(&source[start..end]);
    offset_map.extend(start..end);
}

fn collect_comment_ranges(
    node: Node<'_>,
    comment_kinds: &[&str],
    ranges: &mut Vec<(usize, usize)>,
) {
    if comment_kinds.contains(&node.kind()) {
        ranges.push((node.start_byte(), node.end_byte()));
        return;
    }
    let mut cursor = node.walk();
    for child in node.children(&mut cursor) {
        collect_comment_ranges(child, comment_kinds, ranges);
    }
}

fn collect_structural_events(
    node: Node<'_>,
    structural_depth: usize,
    structural_kinds: &[&str],
    events: &mut Vec<StructuralEvent>,
) {
    let is_structural = structural_kinds.contains(&node.kind());
    if is_structural && node.start_byte() < node.end_byte() {
        events.push(StructuralEvent {
            scope_start: node.start_byte(),
            byte_offset: node.end_byte(),
            depth: structural_depth,
        });
    }

    let child_depth = structural_depth + usize::from(is_structural);
    let mut cursor = node.walk();
    for child in node.children(&mut cursor) {
        collect_structural_events(child, child_depth, structural_kinds, events);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = include_str!("../tests/fixtures/comments.rs");
    const STRUCTURE_FIXTURE: &str = include_str!("../tests/fixtures/structure.rs");
    const C_FIXTURE: &str = include_str!("../tests/fixtures/comments.c");
    const C_STRUCTURE_FIXTURE: &str = include_str!("../tests/fixtures/structure.c");
    const CPP_FIXTURE: &str = include_str!("../tests/fixtures/comments.cc");
    const CPP_STRUCTURE_FIXTURE: &str = include_str!("../tests/fixtures/structure.cc");

    #[test]
    fn strips_line_block_and_doc_comments_with_an_offset_map() {
        let (stripped, map) = RustFrontend.strip_comments(FIXTURE);
        assert!(!stripped.contains("documentation"));
        assert!(!stripped.contains("ordinary comment"));
        assert!(!stripped.contains("block comment"));
        assert!(stripped.contains("fn sample"));
        assert!(stripped.contains("let joined = left right;"));
        assert_eq!(map.len(), stripped.len() + 1);
        assert_eq!(map.last(), Some(&FIXTURE.len()));
        for (byte, original) in stripped.bytes().zip(map.iter().copied()) {
            if byte != b' ' || FIXTURE.as_bytes()[original] == b' ' {
                assert_eq!(byte, FIXTURE.as_bytes()[original]);
            }
        }
    }

    #[test]
    fn strips_outer_and_inner_doc_comments_but_preserves_comment_like_strings() {
        let source = "//! crate docs\n/** item docs */\nfn sample() {\n    let text = \"// not a comment\";\n    /*! block docs */\n}\n";
        let (stripped, map) = RustFrontend.strip_comments(source);
        assert!(!stripped.contains("crate docs"));
        assert!(!stripped.contains("item docs"));
        assert!(!stripped.contains("block docs"));
        assert!(stripped.contains("\"// not a comment\""));
        assert_eq!(map.len(), stripped.len() + 1);
        assert_eq!(map.last(), Some(&source.len()));
    }

    #[test]
    fn reports_required_rust_terminations_and_nesting() {
        let events = RustFrontend.structural_events(STRUCTURE_FIXTURE);
        let end = STRUCTURE_FIXTURE.trim_end().len();
        assert!(
            events
                .iter()
                .any(|event| event.byte_offset == end && event.depth == 0)
        );
        // fn, while, for, loop, if, match, and a bare scope each contribute
        // events, as do their tree-sitter body scopes (`block` or
        // `match_block`).
        assert!(events.len() >= 14);
        assert!(events.iter().any(|event| event.depth >= 4));
        let match_end = STRUCTURE_FIXTURE
            .find("\n    { let _value")
            .expect("bare scope follows match");
        let match_events = events
            .iter()
            .filter(|event| event.byte_offset == match_end)
            .collect::<Vec<_>>();
        assert_eq!(match_events.len(), 2);
        assert_ne!(match_events[0].depth, match_events[1].depth);
        assert!(
            events
                .windows(2)
                .all(|pair| pair[0].byte_offset <= pair[1].byte_offset)
        );
    }

    #[test]
    fn nested_closure_blocks_have_increasing_structural_depth() {
        let source = "fn outer() {\n    let add = |x| {\n        let inner = || {\n            x + 1\n        };\n        inner()\n    };\n}\n";
        let events = RustFrontend.structural_events(source);
        let inner_end = source.find("        };\n").unwrap() + "        }".len();
        let outer_end = source.rfind("    };\n").unwrap() + "    }".len();
        let inner_depth = events
            .iter()
            .find(|event| event.byte_offset == inner_end)
            .unwrap()
            .depth;
        let outer_depth = events
            .iter()
            .find(|event| event.byte_offset == outer_end)
            .unwrap()
            .depth;
        assert!(inner_depth > outer_depth);
    }

    #[test]
    fn impl_body_is_reported_as_a_scope_termination() {
        let source = "struct Counter;\nimpl Counter {\n    fn tick(&self) {}\n}\n";
        let events = RustFrontend.structural_events(source);
        let impl_end = source.rfind('}').unwrap() + 1;
        assert!(events.iter().any(|event| event.byte_offset == impl_end));
    }

    #[test]
    fn struct_and_enum_bodies_are_reported_as_scope_terminations() {
        let source = "struct Point { x: i32 }\nenum Choice { One, Two }\n";
        let events = RustFrontend.structural_events(source);
        let struct_end = source.find('}').unwrap() + 1;
        let enum_end = source.rfind('}').unwrap() + 1;

        for body_end in [struct_end, enum_end] {
            assert!(
                events.iter().any(|event| event.byte_offset == body_end),
                "missing structural event at byte {body_end}"
            );
        }
    }

    #[test]
    fn strips_doc_comments_inside_modules() {
        let source =
            "mod api {\n    //! module docs\n    /// function docs\n    pub fn call() {}\n}\n";
        let (stripped, map) = RustFrontend.strip_comments(source);
        assert!(!stripped.contains("module docs"));
        assert!(!stripped.contains("function docs"));
        assert!(stripped.contains("pub fn call() {}"));
        assert_eq!(map.len(), stripped.len() + 1);
        assert_eq!(map.last(), Some(&source.len()));
    }

    #[test]
    fn preserves_comment_markers_in_raw_strings() {
        let source =
            "fn text() {\n    let value = r##\"// text /* still text */\"##; // remove me\n}\n";
        let (stripped, _) = RustFrontend.strip_comments(source);
        assert!(stripped.contains("r##\"// text /* still text */\"##"));
        assert!(!stripped.contains("remove me"));
    }

    #[test]
    fn unsafe_and_async_blocks_are_nested_scopes() {
        let source = "async fn work() {\n    let task = async {\n        unsafe { do_work(); }\n    };\n    task.await;\n}\n";
        let events = RustFrontend.structural_events(source);
        let unsafe_end = source.find(" }\n").unwrap() + " }".len();
        let async_end = source.find("    };\n").unwrap() + "    }".len();
        let unsafe_depth = events
            .iter()
            .find(|event| event.byte_offset == unsafe_end)
            .unwrap()
            .depth;
        let async_depth = events
            .iter()
            .find(|event| event.byte_offset == async_end)
            .unwrap()
            .depth;
        assert!(unsafe_depth > async_depth);
    }

    fn assert_c_family_comments_are_stripped(frontend: &dyn LanguageFrontend, fixture: &str) {
        let (stripped, map) = frontend.strip_comments(fixture);
        assert!(!stripped.contains("macro-adjacent comment"));
        assert!(!stripped.contains("block comment"));
        assert!(!stripped.contains("join safely"));
        assert!(!stripped.contains("trailing comment"));
        assert!(stripped.contains("#define STRINGIZE(value) #value"));
        assert!(stripped.contains("\"https://example.test/* literal */\""));
        assert!(stripped.contains("left right"));
        assert!(stripped.contains("STRINGIZE(not_a_comment)"));
        assert_eq!(map.len(), stripped.len() + 1);
        assert_eq!(map.last(), Some(&fixture.len()));
        for (byte, original) in stripped.bytes().zip(map.iter().copied()) {
            if byte != b' ' || fixture.as_bytes()[original] == b' ' {
                assert_eq!(byte, fixture.as_bytes()[original]);
            }
        }
    }

    #[test]
    fn strips_c_comments_but_preserves_strings_and_stringized_macros() {
        assert_c_family_comments_are_stripped(&CFrontend, C_FIXTURE);
    }

    #[test]
    fn strips_cpp_comments_but_preserves_string_and_raw_string_literals() {
        assert_c_family_comments_are_stripped(&CppFrontend, CPP_FIXTURE);
        let (stripped, _) = CppFrontend.strip_comments(CPP_FIXTURE);
        assert!(stripped.contains("R\"tag(// raw /* text */)tag\""));
    }

    #[test]
    fn reports_required_c_terminations_and_nested_depths() {
        let events = CFrontend.structural_events(C_STRUCTURE_FIXTURE);
        let end = C_STRUCTURE_FIXTURE.trim_end().len();
        assert!(
            events
                .iter()
                .any(|event| event.byte_offset == end && event.depth == 0)
        );
        assert!(events.len() >= 12);
        assert!(events.iter().any(|event| event.depth >= 7));

        let termination_depth = |needle: &str| {
            let offset = C_STRUCTURE_FIXTURE
                .find(needle)
                .expect("fixture terminator")
                + needle.trim_end_matches('\n').len();
            events
                .iter()
                .filter(|event| event.byte_offset == offset)
                .map(|event| event.depth)
                .min()
                .expect("structural event at fixture terminator")
        };
        let switch_depth = termination_depth("\n                }\n");
        let if_depth = termination_depth("\n            }\n");
        let for_depth = termination_depth("\n        }\n");
        let while_depth = termination_depth("\n    }\n");
        assert!(switch_depth > if_depth);
        assert!(if_depth > for_depth);
        assert!(for_depth > while_depth);
        assert!(
            events
                .windows(2)
                .all(|pair| pair[0].byte_offset <= pair[1].byte_offset)
        );
    }

    #[test]
    fn reports_required_cpp_terminations_and_nested_depths() {
        let events = CppFrontend.structural_events(CPP_STRUCTURE_FIXTURE);
        let end = CPP_STRUCTURE_FIXTURE.trim_end().len();
        assert!(
            events
                .iter()
                .any(|event| event.byte_offset == end && event.depth == 0)
        );
        assert!(events.len() >= 16);
        assert!(events.iter().any(|event| event.depth >= 10));

        let lambda_end = CPP_STRUCTURE_FIXTURE
            .find("        };\n")
            .expect("lambda terminator")
            + "        }".len();
        let lambda_events = events
            .iter()
            .filter(|event| event.byte_offset == lambda_end)
            .collect::<Vec<_>>();
        assert_eq!(lambda_events.len(), 2);
        assert_ne!(lambda_events[0].depth, lambda_events[1].depth);

        let class_end = CPP_STRUCTURE_FIXTURE.find("\n};\n\n").expect("class body") + "\n}".len();
        let try_end = CPP_STRUCTURE_FIXTURE
            .find("\n        }\n    }\n")
            .expect("try/catch body")
            + "\n        }".len();
        let enum_end = CPP_STRUCTURE_FIXTURE
            .find("ready, done }")
            .expect("enumerator list")
            + "ready, done }".len();
        for offset in [class_end, try_end, enum_end] {
            assert!(events.iter().any(|event| event.byte_offset == offset));
        }
    }
}
