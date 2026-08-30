//! Model- and language-independent LM-CC analysis.

use std::collections::{BTreeSet, VecDeque};

use serde::Serialize;
use thiserror::Error;

/// A model token aligned to the preprocessed source.
#[derive(Debug, Clone, PartialEq)]
pub struct Token {
    pub start_byte: usize,
    pub end_byte: usize,
    pub entropy: Option<f64>,
}

/// A syntactic delimiter and the range whose termination it marks.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StructuralEvent {
    /// Start of the syntactic scope in preprocessed-source bytes.
    pub scope_start: usize,
    /// Byte offset immediately after the terminating delimiter.
    pub byte_offset: usize,
    /// Language-frontend nesting depth (zero based).
    pub depth: usize,
}

/// One node in the semantic compositional hierarchy.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Unit {
    pub start_byte: usize,
    pub end_byte: usize,
    pub level: u64,
    pub branching: u64,
    pub children: Vec<Unit>,
}

/// Complete LM-CC output.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Analysis {
    pub lmcc: f64,
    pub total_branch: u64,
    pub total_comp_level: u64,
    pub alpha: f64,
    pub tau: f64,
    pub units: Vec<Unit>,
}

/// A flat entropy/structure semantic unit used by Algorithm 1.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SemanticUnit {
    pub start_byte: usize,
    pub end_byte: usize,
    pub nesting_depth: usize,
}

#[derive(Debug, Error, PartialEq)]
pub enum AnalysisError {
    #[error("tau percentile must be finite and between 0 and 100")]
    InvalidPercentile,
    #[error("alpha must be finite and between 0 and 1")]
    InvalidAlpha,
    #[error("token entropy must be finite and non-negative")]
    InvalidEntropy,
    #[error("token byte ranges must be ordered, non-overlapping, and non-empty")]
    InvalidTokenRanges,
    #[error("structural event range is invalid")]
    InvalidStructuralEvent,
}

/// Computes a linearly interpolated percentile over finite values.
///
/// The rank is `p / 100 * (n - 1)`, with interpolation between adjacent
/// samples. `None` is returned for an empty sample.
pub fn percentile(values: &[f64], percentile: f64) -> Result<Option<f64>, AnalysisError> {
    if !percentile.is_finite() || !(0.0..=100.0).contains(&percentile) {
        return Err(AnalysisError::InvalidPercentile);
    }
    if values.is_empty() {
        return Ok(None);
    }
    if values.iter().any(|value| !value.is_finite()) {
        return Err(AnalysisError::InvalidEntropy);
    }

    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let rank = percentile / 100.0 * (sorted.len() - 1) as f64;
    let lower = rank.floor() as usize;
    let upper = rank.ceil() as usize;
    let fraction = rank - lower as f64;
    Ok(Some(
        sorted[lower] + (sorted[upper] - sorted[lower]) * fraction,
    ))
}

/// Detects entropy and structural boundaries and returns the threshold and
/// resulting flat semantic units.
pub fn detect_semantic_units(
    tokens: &[Token],
    structural_events: &[StructuralEvent],
    tau_percentile: f64,
) -> Result<(f64, Vec<SemanticUnit>), AnalysisError> {
    validate_inputs(tokens, structural_events)?;

    let entropy_values = tokens
        .iter()
        .filter_map(|token| token.entropy)
        .collect::<Vec<_>>();
    if entropy_values
        .iter()
        .any(|entropy| !entropy.is_finite() || *entropy < 0.0)
    {
        return Err(AnalysisError::InvalidEntropy);
    }
    let tau = percentile(&entropy_values, tau_percentile)?.unwrap_or(0.0);
    if tokens.is_empty() {
        return Ok((tau, Vec::new()));
    }

    // Boundaries are token indices. Entropy H(t_i) starts a unit at t_i;
    // a structural termination ends the unit containing the delimiter.
    let mut boundaries = BTreeSet::from([0, tokens.len()]);
    for (index, token) in tokens.iter().enumerate().skip(1) {
        if token.entropy.is_some_and(|entropy| entropy > tau) {
            boundaries.insert(index);
        }
    }
    for event in structural_events {
        let after_delimiter = tokens.partition_point(|token| token.end_byte < event.byte_offset);
        let boundary = if after_delimiter < tokens.len()
            && tokens[after_delimiter].start_byte < event.byte_offset
        {
            after_delimiter + 1
        } else {
            after_delimiter
        };
        boundaries.insert(boundary);
    }

    let boundary_indices = boundaries.into_iter().collect::<Vec<_>>();
    let mut units = Vec::with_capacity(boundary_indices.len().saturating_sub(1));
    for pair in boundary_indices.windows(2) {
        let start_index = pair[0];
        let end_index = pair[1];
        if start_index == end_index {
            continue;
        }
        let start_byte = tokens[start_index].start_byte;
        let end_byte = tokens[end_index - 1].end_byte;
        let nesting_depth = structural_events
            .iter()
            .filter(|event| event.scope_start <= start_byte && start_byte < event.byte_offset)
            .map(|event| event.depth)
            .max()
            .unwrap_or(0);
        units.push(SemanticUnit {
            start_byte,
            end_byte,
            nesting_depth,
        });
    }
    Ok((tau, units))
}

/// Constructs the paper's hierarchy with a breadth-first traversal.
pub fn build_hierarchy(semantic_units: &[SemanticUnit]) -> Vec<Unit> {
    if semantic_units.is_empty() {
        return Vec::new();
    }

    #[derive(Debug)]
    struct ArenaUnit {
        first: usize,
        end: usize,
        level: u64,
        children: Vec<usize>,
    }

    let root_ranges = partition_at_shallowest(semantic_units, 0, semantic_units.len());
    let mut arena = root_ranges
        .iter()
        .map(|&(first, end)| ArenaUnit {
            first,
            end,
            level: 1,
            children: Vec::new(),
        })
        .collect::<Vec<_>>();
    let roots = (0..arena.len()).collect::<Vec<_>>();
    let mut queue = roots.iter().copied().collect::<VecDeque<_>>();
    while let Some(parent_index) = queue.pop_front() {
        let first = arena[parent_index].first;
        let end = arena[parent_index].end;
        if end - first <= 1 {
            continue;
        }

        // The first semantic unit anchors this node. Its more deeply nested
        // suffix is partitioned into children at the next compositional level.
        for (child_first, child_end) in partition_at_shallowest(semantic_units, first + 1, end) {
            let child_index = arena.len();
            arena.push(ArenaUnit {
                first: child_first,
                end: child_end,
                level: arena[parent_index].level + 1,
                children: Vec::new(),
            });
            arena[parent_index].children.push(child_index);
            queue.push_back(child_index);
        }
    }

    fn materialize(index: usize, arena: &[ArenaUnit], leaves: &[SemanticUnit]) -> Unit {
        let node = &arena[index];
        Unit {
            start_byte: leaves[node.first].start_byte,
            end_byte: leaves[node.end - 1].end_byte,
            level: node.level,
            branching: node.children.len() as u64,
            children: node
                .children
                .iter()
                .map(|child| materialize(*child, arena, leaves))
                .collect(),
        }
    }

    roots
        .into_iter()
        .map(|root| materialize(root, &arena, semantic_units))
        .collect()
}

/// Runs boundary detection, Algorithm 1, and LM-CC aggregation.
pub fn analyze(
    tokens: &[Token],
    structural_events: &[StructuralEvent],
    tau_percentile: f64,
    alpha: f64,
) -> Result<Analysis, AnalysisError> {
    if !alpha.is_finite() || !(0.0..=1.0).contains(&alpha) {
        return Err(AnalysisError::InvalidAlpha);
    }
    let (tau, semantic_units) = detect_semantic_units(tokens, structural_events, tau_percentile)?;
    let units = build_hierarchy(&semantic_units);
    let (total_branch, total_comp_level) = totals(&units);
    let lmcc = alpha * total_branch as f64 + (1.0 - alpha) * total_comp_level as f64;
    Ok(Analysis {
        lmcc,
        total_branch,
        total_comp_level,
        alpha,
        tau,
        units,
    })
}

fn partition_at_shallowest(
    units: &[SemanticUnit],
    first: usize,
    end: usize,
) -> Vec<(usize, usize)> {
    if first >= end {
        return Vec::new();
    }
    let shallowest = units[first..end]
        .iter()
        .map(|unit| unit.nesting_depth)
        .min()
        .expect("non-empty semantic-unit range");
    let starts = (first..end)
        .filter(|index| units[*index].nesting_depth == shallowest)
        .collect::<Vec<_>>();
    starts
        .iter()
        .enumerate()
        .map(|(index, start)| (*start, starts.get(index + 1).copied().unwrap_or(end)))
        .collect()
}

fn totals(units: &[Unit]) -> (u64, u64) {
    units.iter().fold((0, 0), |(branches, levels), unit| {
        let (child_branches, child_levels) = totals(&unit.children);
        (
            branches + unit.branching + child_branches,
            levels + unit.level + child_levels,
        )
    })
}

fn validate_inputs(
    tokens: &[Token],
    structural_events: &[StructuralEvent],
) -> Result<(), AnalysisError> {
    let mut previous_end = 0;
    for (index, token) in tokens.iter().enumerate() {
        if token.start_byte >= token.end_byte || (index > 0 && token.start_byte < previous_end) {
            return Err(AnalysisError::InvalidTokenRanges);
        }
        previous_end = token.end_byte;
    }
    let source_end = tokens.last().map_or(0, |token| token.end_byte);
    if structural_events
        .iter()
        .any(|event| event.scope_start >= event.byte_offset || event.byte_offset > source_end)
    {
        return Err(AnalysisError::InvalidStructuralEvent);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn byte_tokens(entropies: &[Option<f64>]) -> Vec<Token> {
        entropies
            .iter()
            .enumerate()
            .map(|(start_byte, entropy)| Token {
                start_byte,
                end_byte: start_byte + 1,
                entropy: *entropy,
            })
            .collect()
    }

    #[test]
    fn percentile_and_tau_handle_degenerate_entropy_samples() {
        assert_eq!(percentile(&[], 67.0).unwrap(), None);
        for percentile_rank in [0.0, 67.0, 100.0] {
            assert_eq!(
                percentile(&[2.5, 2.5, 2.5], percentile_rank).unwrap(),
                Some(2.5)
            );
        }

        let all_equal = byte_tokens(&[Some(2.5), Some(2.5), Some(2.5)]);
        let (tau, units) = detect_semantic_units(&all_equal, &[], 67.0).unwrap();
        assert_eq!(tau, 2.5);
        assert_eq!(units.len(), 1, "equal-to-tau entropy is not a boundary");

        let single = byte_tokens(&[Some(4.0)]);
        let (tau, units) = detect_semantic_units(&single, &[], 67.0).unwrap();
        assert_eq!(tau, 4.0);
        assert_eq!(
            units,
            vec![SemanticUnit {
                start_byte: 0,
                end_byte: 1,
                nesting_depth: 0,
            }]
        );

        let only_nulls = byte_tokens(&[None, None]);
        let (tau, units) = detect_semantic_units(&only_nulls, &[], 67.0).unwrap();
        assert_eq!(tau, 0.0);
        assert_eq!(
            units,
            vec![SemanticUnit {
                start_byte: 0,
                end_byte: 2,
                nesting_depth: 0,
            }]
        );
    }

    #[test]
    fn percentile_interpolates_and_boundaries_are_strict() {
        let tokens = vec![
            Token {
                start_byte: 0,
                end_byte: 1,
                entropy: None,
            },
            Token {
                start_byte: 1,
                end_byte: 2,
                entropy: Some(1.0),
            },
            Token {
                start_byte: 2,
                end_byte: 3,
                entropy: Some(2.0),
            },
            Token {
                start_byte: 3,
                end_byte: 4,
                entropy: Some(3.0),
            },
        ];
        let (tau, units) = detect_semantic_units(&tokens, &[], 50.0).unwrap();
        assert_eq!(tau, 2.0);
        assert_eq!(units.len(), 2);
        assert_eq!((units[0].start_byte, units[0].end_byte), (0, 3));
        assert_eq!((units[1].start_byte, units[1].end_byte), (3, 4));
        assert_eq!(percentile(&[0.0, 10.0], 67.0).unwrap(), Some(6.7));
    }

    #[test]
    fn structural_termination_splits_after_its_token() {
        let tokens = vec![
            Token {
                start_byte: 0,
                end_byte: 2,
                entropy: Some(0.0),
            },
            Token {
                start_byte: 2,
                end_byte: 5,
                entropy: Some(0.0),
            },
            Token {
                start_byte: 5,
                end_byte: 7,
                entropy: Some(0.0),
            },
        ];
        let events = vec![StructuralEvent {
            scope_start: 0,
            byte_offset: 4,
            depth: 1,
        }];
        let (_, units) = detect_semantic_units(&tokens, &events, 67.0).unwrap();
        assert_eq!(
            units
                .iter()
                .map(|unit| (unit.start_byte, unit.end_byte))
                .collect::<Vec<_>>(),
            vec![(0, 5), (5, 7)]
        );
    }

    #[test]
    fn entropy_spikes_and_structural_terminations_are_unioned() {
        let tokens = (0..5)
            .map(|start_byte| Token {
                start_byte,
                end_byte: start_byte + 1,
                entropy: Some(if start_byte == 2 { 10.0 } else { 0.0 }),
            })
            .collect::<Vec<_>>();
        let events = vec![StructuralEvent {
            scope_start: 0,
            byte_offset: 4,
            depth: 0,
        }];
        let (tau, units) = detect_semantic_units(&tokens, &events, 67.0).unwrap();
        assert_eq!(tau, 0.0);
        assert_eq!(
            units
                .iter()
                .map(|unit| (unit.start_byte, unit.end_byte))
                .collect::<Vec<_>>(),
            vec![(0, 2), (2, 4), (4, 5)]
        );
    }

    #[test]
    fn coincident_and_repeated_boundaries_are_deduplicated() {
        let tokens = byte_tokens(&[None, Some(0.0), Some(10.0), Some(0.0)]);
        let event = StructuralEvent {
            scope_start: 0,
            byte_offset: 2,
            depth: 0,
        };
        let (_, units) = detect_semantic_units(&tokens, &[event.clone(), event], 67.0).unwrap();
        assert_eq!(
            units
                .iter()
                .map(|unit| (unit.start_byte, unit.end_byte))
                .collect::<Vec<_>>(),
            vec![(0, 2), (2, 4)]
        );
    }

    #[test]
    fn bfs_hierarchy_has_known_depth_and_branch_totals() {
        let leaves = vec![
            SemanticUnit {
                start_byte: 0,
                end_byte: 1,
                nesting_depth: 0,
            },
            SemanticUnit {
                start_byte: 1,
                end_byte: 2,
                nesting_depth: 1,
            },
            SemanticUnit {
                start_byte: 2,
                end_byte: 3,
                nesting_depth: 1,
            },
            SemanticUnit {
                start_byte: 3,
                end_byte: 4,
                nesting_depth: 0,
            },
        ];
        let units = build_hierarchy(&leaves);
        let (branches, levels) = totals(&units);
        assert_eq!((branches, levels), (2, 6));
        assert_eq!(units.len(), 2);
        assert_eq!(units[0].level, 1);
        assert_eq!(units[0].branching, 2);
        assert_eq!(units[0].children[0].level, 2);
        assert_eq!(units[1].level, 1);
        assert_eq!(units[1].branching, 0);
        let score = 0.8 * branches as f64 + 0.2 * levels as f64;
        assert!((score - 2.8).abs() < 1e-12);
    }

    #[test]
    fn top_level_semantic_units_are_roots_not_children_of_an_artificial_node() {
        let leaves = (0..3)
            .map(|start_byte| SemanticUnit {
                start_byte,
                end_byte: start_byte + 1,
                nesting_depth: 0,
            })
            .collect::<Vec<_>>();
        let units = build_hierarchy(&leaves);
        assert_eq!(units.len(), 3);
        assert!(
            units
                .iter()
                .all(|unit| unit.level == 1 && unit.branching == 0)
        );
        assert_eq!(totals(&units), (0, 3));
    }

    #[test]
    fn deeply_nested_units_form_the_expected_chain() {
        let leaves = (0..64)
            .map(|start_byte| SemanticUnit {
                start_byte,
                end_byte: start_byte + 1,
                nesting_depth: start_byte,
            })
            .collect::<Vec<_>>();
        let units = build_hierarchy(&leaves);
        assert_eq!(units.len(), 1);

        let mut unit = &units[0];
        for expected_level in 1..=64 {
            assert_eq!(unit.level, expected_level);
            assert_eq!(unit.start_byte, (expected_level - 1) as usize);
            assert_eq!(unit.end_byte, 64);
            let expected_branching = u64::from(expected_level < 64);
            assert_eq!(unit.branching, expected_branching);
            if let Some(child) = unit.children.first() {
                unit = child;
            }
        }
        assert_eq!(totals(&units), (63, 2080));
    }

    #[test]
    fn nontrivial_hierarchy_matches_the_golden_tree() {
        let leaves = [0, 1, 2, 2, 1, 2, 0, 1]
            .into_iter()
            .enumerate()
            .map(|(start_byte, nesting_depth)| SemanticUnit {
                start_byte,
                end_byte: start_byte + 1,
                nesting_depth,
            })
            .collect::<Vec<_>>();

        let units = build_hierarchy(&leaves);
        assert_eq!(
            units,
            vec![
                Unit {
                    start_byte: 0,
                    end_byte: 6,
                    level: 1,
                    branching: 2,
                    children: vec![
                        Unit {
                            start_byte: 1,
                            end_byte: 4,
                            level: 2,
                            branching: 2,
                            children: vec![
                                Unit {
                                    start_byte: 2,
                                    end_byte: 3,
                                    level: 3,
                                    branching: 0,
                                    children: vec![],
                                },
                                Unit {
                                    start_byte: 3,
                                    end_byte: 4,
                                    level: 3,
                                    branching: 0,
                                    children: vec![],
                                },
                            ],
                        },
                        Unit {
                            start_byte: 4,
                            end_byte: 6,
                            level: 2,
                            branching: 1,
                            children: vec![Unit {
                                start_byte: 5,
                                end_byte: 6,
                                level: 3,
                                branching: 0,
                                children: vec![],
                            }],
                        },
                    ],
                },
                Unit {
                    start_byte: 6,
                    end_byte: 8,
                    level: 1,
                    branching: 1,
                    children: vec![Unit {
                        start_byte: 7,
                        end_byte: 8,
                        level: 2,
                        branching: 0,
                        children: vec![],
                    }],
                },
            ]
        );
        assert_eq!(totals(&units), (6, 17));
    }

    #[test]
    fn scoring_respects_half_and_branch_only_alpha_values() {
        let tokens = byte_tokens(&[None, Some(0.0), Some(10.0), Some(0.0)]);
        let events = [StructuralEvent {
            scope_start: 2,
            byte_offset: 4,
            depth: 1,
        }];

        let half = analyze(&tokens, &events, 67.0, 0.5).unwrap();
        assert_eq!((half.total_branch, half.total_comp_level), (1, 3));
        assert_eq!(half.lmcc, 2.0);

        let branch_only = analyze(&tokens, &events, 67.0, 1.0).unwrap();
        assert_eq!(
            (branch_only.total_branch, branch_only.total_comp_level),
            (1, 3)
        );
        assert_eq!(branch_only.lmcc, 1.0);
    }
}
