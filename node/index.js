const path = require('path');
const addon = require(path.join(__dirname, 'build', 'Release', 'flox_node.node'));

module.exports = addon;
