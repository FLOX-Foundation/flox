const path = require('path');

const key = `${process.platform}-${process.arch}`;
const prebuilt = path.join(__dirname, 'prebuilds', key, 'lrvx_node.node');
const local = path.join(__dirname, 'build', 'Release', 'lrvx_node.node');

let native;
try {
  native = require(prebuilt);
} catch (_) {
  native = require(local);
}

const composite = require('./lib/composite');
const dex = require('./lib/dex');

module.exports = Object.assign({}, native, { composite, dex });
