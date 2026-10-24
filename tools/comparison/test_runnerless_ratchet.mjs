// Verify release registration and retired comparisons against the versioned host ABI:
// node tools/comparison/test_runnerless_ratchet.mjs PROGRAM.wasm
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const wasm = await readFile(process.argv[2]);
const encoder = new TextEncoder(), decoder = new TextDecoder();
const head = 'a'.repeat(40), base = 'b'.repeat(40);
async function run({ details = {} } = {}) {
  let exports, response = new Uint8Array(), operations = 0;
  const checks = [], workflows = [], titlePolicies = [], recipes = new Map();
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
        if (kind === 1) assert.fail('retired comparisons must not read producer files');
        if (kind === 4) { response = encoder.encode(event[request[0]] ?? ''); return; }
        assert.equal(kind, 8);
        const { capability, args } = request;
        let value = null, ok = true;
        if (capability === 'event.details') value = event;
        else if (capability === 'configure.workflow') workflows.push(args);
        else if (capability === 'configure.titlePolicy') titlePolicies.push(args);
        else if (capability === 'jobs.result') ok = false;
        else if (capability === 'workflow.configure' || capability === 'workflow.run') {
          assert.ok(!recipes.has(capability), `${capability} called more than once`);
          recipes.set(capability, args);
        }
        else assert.ok(['configure.badge', 'workflow.artifacts.configure',
          'workflow.artifacts.run'].includes(capability), capability);
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
  assert.deepEqual(titlePolicies, [{ enabled: true, format: 'conventional-commits' }]);
  assert.deepEqual(recipes.get('workflow.configure'), { version: 1, release: true, deploy: null },
    'Release Please must be enabled in the repository program; YAML only supplies its settings');
  assert.equal(workflows.find(item => item.id === 'llm-cc-token-ratio'), undefined);
  operations = 0; // Configuration and execution have separate production budgets.
  exports.run();
  assert.deepEqual(recipes.get('workflow.run'), recipes.get('workflow.configure'),
    'Execution must use the configured release recipe');
  assert.deepEqual(checks.filter(check => check[0] === 'LM-CC/token'), [],
    'Retired GPU comparisons must not publish a merge gate');
}

for (const event of ['pull_request', 'status', 'issue_comment', 'runnerless_completion']) {
  await run({ details: { event } });
}
await run({ details: { event: 'push', pullNumber: null } });
console.log('Runnerless Wasm: release registration and GPU comparison retirement passed for five event types.');
