#!/usr/bin/env node
'use strict';

const { spawn } = require('node:child_process');
const { version, optionalDependencies } = require('./package.json');
const name = `@pawelchcki/llm-cc-${process.platform}-${process.arch}`;
const executable = process.platform === 'win32' ? 'llm-cc.exe' : 'llm-cc';

let binary;
try {
  if (!Object.hasOwn(optionalDependencies, name)) {
    throw new Error(`Unsupported platform: ${process.platform}-${process.arch}`);
  }
  binary = require.resolve(`${name}/bin/${executable}`);
  if (require(`${name}/package.json`).version !== version) {
    throw new Error('The binary package version does not match the wrapper');
  }
} catch (error) {
  console.error(`llm-cc: ${error.message}\nReinstall llm-cc with optional dependencies enabled (npm install --include=optional llm-cc).`);
  process.exit(1);
}

const child = spawn(binary, process.argv.slice(2), { stdio: 'inherit' });
child.on('error', (error) => {
  console.error(`llm-cc: ${error.message}`);
  process.exitCode = 1;
});
// Keep the wrapper alive during termination so its child can clean up.
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => child.kill(signal));
}
child.on('exit', (code, signal) => {
  if (signal) {
    process.removeAllListeners(signal);
    process.kill(process.pid, signal);
  } else {
    process.exitCode = code;
  }
});
