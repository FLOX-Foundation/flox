const path = require('path');

const key = `${process.platform}-${process.arch}`;
const prebuilt = path.join(__dirname, 'prebuilds', key, 'flox_node.node');
const local = path.join(__dirname, 'build', 'Release', 'flox_node.node');

let native;
try {
  native = require(prebuilt);
} catch (_) {
  native = require(local);
}

const composite = require('./lib/composite');

module.exports = Object.assign({}, native, { composite });
