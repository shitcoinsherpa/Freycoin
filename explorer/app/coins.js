"use strict";

const btc = require("./coins/btc.js");
const frey = require("./coins/freycoin.js");

module.exports = {
	"BTC": btc,
	"FREY": frey,

	"coins":["BTC", "FREY"]
};
