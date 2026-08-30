//! Language frontends for LM-CC.

pub use lmcc_core::StructuralEvent;
use tree_sitter::{Node, Parser};

/// Maps every preprocessed byte boundary to a byte boundary in the original
/// source. Its length is always `preprocessed.len() + 1`.
pub type OffsetMap = Vec<usize>;

pub trait LanguageFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap);
    fn structural_events(&self, src: &str) -> Vec<StructuralEvent>;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct RustFrontend;

impl RustFrontend {
    fn parser() -> Parser {
        let mut parser = Parser::new();
        parser
            .set_language(&tree_sitter_rust::LANGUAGE.into())
            .expect("tree-sitter-rust language must load");
        parser
    }
}

impl LanguageFrontend for RustFrontend {
    fn strip_comments(&self, src: &str) -> (String, OffsetMap) {
        let mut parser = Self::parser();
        let tree = parser
            .parse(src, None)
            .expect("tree-sitter parser returned no tree");
        let mut comment_ranges = Vec::new();
        collect_comment_ranges(tree.root_node(), &mut comment_ranges);
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

    fn structural_events(&self, src: &str) -> Vec<StructuralEvent> {
        let mut parser = Self::parser();
        let tree = parser
            .parse(src, None)
            .expect("tree-sitter parser returned no tree");
        let mut events = Vec::new();
        collect_structural_events(tree.root_node(), 0, &mut events);
        events.sort_unstable_by_key(|event| (event.byte_offset, event.depth, event.scope_start));
        events.dedup();
        events
    }
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

fn collect_comment_ranges(node: Node<'_>, ranges: &mut Vec<(usize, usize)>) {
    if matches!(node.kind(), "line_comment" | "block_comment") {
        ranges.push((node.start_byte(), node.end_byte()));
        return;
    }
    let mut cursor = node.walk();
    for child in node.children(&mut cursor) {
        collect_comment_ranges(child, ranges);
    }
}

fn collect_structural_events(
    node: Node<'_>,
    structural_depth: usize,
    events: &mut Vec<StructuralEvent>,
) {
    let is_structural = matches!(
        node.kind(),
        "function_item"
            | "loop_expression"
            | "while_expression"
            | "for_expression"
            | "if_expression"
            | "match_expression"
            | "match_block"
            | "block"
            | "declaration_list"
    );
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
        collect_structural_events(child, child_depth, events);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = include_str!("../tests/fixtures/comments.rs");
    const STRUCTURE_FIXTURE: &str = include_str!("../tests/fixtures/structure.rs");

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
}
