// Run the compiled .runnerless-ci.ts against the versioned host ABI:
// node tools/comparison/test_runnerless_ratchet.mjs PROGRAM.wasm
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const wasm = await readFile(process.argv[2]);
const encoder = new TextEncoder(), decoder = new TextDecoder();
const head = 'a'.repeat(40), base = 'b'.repeat(40);
const data = { schema_version: 1, status: 'complete', identity: {
  repository: 'pawelchcki/llm-cc', pr_number: 42, head_sha: head, target_sha: base,
}, base_ratio: 0.1, head_ratio: 0.1 };
const publisher = '<!-- codex-review-artifact-publisher -->';
const artifact = '<!-- ci-toolkit:complexity -->';
const producers = ['buildbuddy.yaml', 'tools/comparison/dogfood.sh',
  'tools/comparison/ratchet.py', '.llm-cc/rules.json'];
function comment(payload = data, actorId = 317265358, attribution = '') {
  return { actorId, body: publisher + '\n' + attribution + artifact +
    '\nSummary\nLM-CC/token data: `' + JSON.stringify(payload) + '`\n' };
}

async function run(items, { proposed = '', paged = false, details = {}, pull = {} } = {}) {
  let exports, response = new Uint8Array(), operations = 0;
  const checks = [], workflows = [];
  const event = { event: 'pull_request', action: 'opened', state: 'open',
    pullNumber: 42, fullName: 'pawelchcki/llm-cc', head, base, ...details };
  const { instance } = await WebAssembly.instantiate(wasm, {
    env: { abort(_message, _file, line, column) {
      throw new Error(`Wasm abort at ${line}:${column}`);
    } },
    runnerless_v1: {
      submit(kind, ptr, length) {
        assert.ok(++operations <= 32, 'Runnerless host-operation budget exceeded');
        const request = JSON.parse(decoder.decode(new Uint8Array(exports.memory.buffer, ptr, length)));
        if (kind === 3) { checks.push(request); response = encoder.encode('null'); return; }
        if (kind === 1) {
          const [path, ref] = request;
          assert.ok(producers.includes(path), path);
          response = encoder.encode(path === proposed && ref === 'head' ? 'changed' : 'trusted');
          return;
        }
        if (kind === 4) { response = encoder.encode(event[request[0]] ?? ''); return; }
        assert.equal(kind, 8);
        const { capability, args } = request;
        let value = null, ok = true;
        if (capability === 'event.details') value = event;
        else if (capability === 'pulls.get') value = { number: 42, state: 'open', head, base, ...pull };
        else if (capability === 'comments.list') {
          const all = paged ? [{ actorId: 1, body: 'unrelated' }, ...items] : items;
          const first = (args.page - 1) * args.size;
          const selected = all.slice(first, first + args.size);
          value = { items: selected, nextPage: selected.length === args.size ? args.page + 1 : null };
        }
        else if (capability === 'configure.workflow') workflows.push(args);
        else if (capability === 'jobs.result') ok = false;
        else assert.ok(['configure.badge', 'workflow.configure', 'workflow.artifacts.configure',
          'workflow.run', 'workflow.artifacts.run'].includes(capability), capability);
        response = encoder.encode(JSON.stringify({ version: 1, ok, value }));
      },
      poll() { return response.length; },
      copy(ptr, capacity) {
        assert.ok(capacity >= response.length);
        new Uint8Array(exports.memory.buffer, ptr, response.length).set(response);
        return response.length;
      },
    },
  });
  exports = instance.exports;
  exports.__runnerless_init();
  exports.configure();
  assert.deepEqual(workflows.find(item => item.id === 'llm-cc-token-ratio').events,
    ['pull_request', 'status', 'runnerless_completion', 'issue_comment']);
  operations = 0; // Configuration and execution have separate production budgets.
  exports.run();
  const ratios = checks.filter(check => check[0] === 'LM-CC/token');
  if (event.pullNumber === null) { assert.equal(ratios.length, 0); return null; }
  assert.equal(ratios.length, 1);
  return ratios[0][1];
}

let scenarios = 0;
async function expect(items, passed, options) {
  assert.equal(await run(items, options), passed);
  scenarios++;
}
for (const [ratio, passed] of [[0.1, true], [0.05, true], [0.100000000001, false],
  [null, false], [-0.1, false], [Infinity, false], [true, false], ['0.05', false]]) {
  await expect([comment({ ...data, head_ratio: ratio })], passed);
}
await expect([comment({ ...data, base_ratio: 0, head_ratio: 0 })], true);
await expect([comment({ ...data, base_ratio: 0, head_ratio: 0.1 })], false);
for (const status of ['incomplete', 'failed']) await expect([comment({ ...data, status })], false);
await expect([comment({ ...data, schema_version: 2 })], false);
await expect([], false);
await expect([comment(data, 1)], false);
await expect([{ actorId: 317265358, body: 'Unrelated', other: comment().body }], false);
await expect([{ actorId: 317265358, body: 'Unrelated "body":"' + comment().body }], false);
await expect([{ actorId: 317265358, body: publisher + artifact },
  { actorId: 317265358, body: publisher + '<!-- ci-toolkit:gpu-backends -->\n' + 'x'.repeat(4000) },
  { actorId: 1, body: 'Unrelated\n'.repeat(200) }], false);
await expect([{ ...comment(), body: 'unverified ' + comment().body }], false);
await expect([{ ...comment(), body: comment().body.replace(artifact, '<!-- ci-toolkit:gpu-backends -->') }], false);
for (const [field, value] of [['repository', 'other/repo'], ['pr_number', 43],
  ['head_sha', 'c'.repeat(40)], ['target_sha', 'c'.repeat(40)]]) {
  await expect([comment({ ...data, identity: { ...data.identity, [field]: value } })], false);
}
await expect([comment()], true, { paged: true });
await expect([comment(data, 317265358, '> Automation policy from the trusted base.\n\n')], true);
const large = comment();
large.body = large.body.replace('Summary', '### llm-cc: LM-CC Δ +554.6 · LM-CC/token 0.110017 → 0.109755\n' +
  '| `src/parser.cc` | runtime | +35.6 |\n'.repeat(650));
await expect([large], true);
await expect([{ actorId: 317265358, body: 'Unrelated automation\n'.repeat(1000) }, large], true);
await expect([comment(data, 317265358, 'Unicode: Δ → 🦀, escapes: " \\ \t \r \b \f\n')], true);
await expect([large], true, { pull: { title: 'feat: ratio gate', body: 'PR details\n'.repeat(500) } });
await expect([large], true, { pull: { body: 'Details\n'.repeat(6000) } });
for (const proposed of producers) await expect([comment()], false, { proposed });
await expect([comment()], true, { details: { event: 'runnerless_completion' } });
await expect([], null, { details: { event: 'push', pullNumber: null } });
console.log(`Runnerless Wasm: ${scenarios} ratio, identity, publisher, pagination, producer and event scenarios passed.`);
