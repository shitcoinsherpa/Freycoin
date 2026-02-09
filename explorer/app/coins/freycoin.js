"use strict";

const Decimal = require("decimal.js");
const Decimal8 = Decimal.clone({ precision:8, rounding:8 });

// Block reward schedule: starts at 50, halves every 840,000 blocks
// Tail emission of 0.1 FREY after final halving
const blockRewardEras = [ new Decimal8(50) ];
for (let i = 1; i < 34; i++) {
	let previous = blockRewardEras[i - 1];
	let halved = new Decimal8(previous).dividedBy(2);

	// Tail emission floor: 0.1 FREY
	if (halved.lessThan(new Decimal8("0.1"))) {
		halved = new Decimal8("0.1");
	}

	blockRewardEras.push(halved);
}

const currencyUnits = [
	{
		type:"native",
		name:"FREY",
		multiplier:1,
		default:true,
		values:["", "frey", "FREY"],
		decimalPlaces:8
	},
	{
		type:"native",
		name:"mFREY",
		multiplier:1000,
		values:["mfrey"],
		decimalPlaces:5
	},
	{
		type:"native",
		name:"frey",
		multiplier:100000000,
		values:["subfrey", "base"],
		decimalPlaces:0
	},
];

module.exports = {
	name:"Freycoin",
	ticker:"FREY",
	logoUrlsByNetwork:{
		"main":"./img/network-mainnet/logo.svg",
		"test":"./img/network-testnet/logo.svg",
		"regtest":"./img/network-regtest/logo.svg",
	},
	coinIconUrlsByNetwork:{
		"main":"./img/network-mainnet/coin-icon.svg",
		"test":"./img/network-testnet/coin-icon.svg",
		"regtest":"./img/network-regtest/coin-icon.svg",
	},
	coinColorsByNetwork: {
		"main": "#6B46C1",
		"test": "#1daf00",
		"regtest": "#777"
	},
	siteTitlesByNetwork: {
		"main":"Freycoin Explorer",
		"test":"Freycoin Testnet Explorer",
		"regtest":"Freycoin Regtest Explorer",
	},
	demoSiteUrlsByNetwork: {
		"main": "https://explorer.freycoin.tech",
		"test": "https://testnet.freycoin.tech",
	},
	knownTransactionsByNetwork: {
	},
	miningPoolsConfigUrls:[],
	maxBlockWeight: 4000000,
	maxBlockSize: 1000000,
	minTxBytes: 166,
	minTxWeight: 166 * 4,
	difficultyAdjustmentBlockCount: 144,
	maxSupplyByNetwork: {
		"main": new Decimal(84000000),
		"test": new Decimal(84000000),
		"regtest": new Decimal(84000000),
	},
	targetBlockTimeSeconds: 150,
	targetBlockTimeMinutes: 2.5,
	currencyUnits:currencyUnits,
	currencyUnitsByName:{"FREY":currencyUnits[0], "mFREY":currencyUnits[1], "frey":currencyUnits[2]},
	baseCurrencyUnit:currencyUnits[2],
	defaultCurrencyUnit:currencyUnits[0],
	feeSatoshiPerByteBucketMaxima: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 50, 75, 100, 150],

	halvingBlockIntervalsByNetwork: {
		"main": 840000,
		"test": 840000,
		"regtest": 150,
	},

	terminalHalvingCountByNetwork: {
		"main": 32,
		"test": 32,
		"regtest": 32,
	},

	coinSupplyCheckpointsByNetwork: {
		"main": [ 0, new Decimal(0) ],
		"test": [ 0, new Decimal(0) ],
		"regtest": [ 0, new Decimal(0) ]
	},

	utxoSetCheckpointsByNetwork: {},

	genesisBlockHashesByNetwork:{
		"main":	"0000000000000000000000000000000000000000000000000000000000000000",
		"test":	"bf0bc1c9f3a5dcaa07b498707469f3c53104b015a0034b84bf978ae2c8ea9a69",
		"regtest": "0000000000000000000000000000000000000000000000000000000000000000",
	},
	genesisCoinbaseTransactionIdsByNetwork: {
		"main":	"0000000000000000000000000000000000000000000000000000000000000000",
		"test":	"3d88cf475c00c0a831fb98c7816aa8ad8dae0edcaca6d012cbb4cef3bc6402d5",
		"regtest": "0000000000000000000000000000000000000000000000000000000000000000",
	},
	genesisCoinbaseTransactionsByNetwork:{
		"test": {
			"txid": "3d88cf475c00c0a831fb98c7816aa8ad8dae0edcaca6d012cbb4cef3bc6402d5",
			"hash": "3d88cf475c00c0a831fb98c7816aa8ad8dae0edcaca6d012cbb4cef3bc6402d5",
			"version": 1,
			"size": 233,
			"vsize": 233,
			"weight": 932,
			"locktime": 0,
			"vin": [
				{
					"coinbase": "04ffff001d01044c4d4a6f6e6e6965204672657920313938392d323031372e2045766572792070726f6f66206f6620776f726b2061647661636573206d617468656d61746963616c206b6e6f776c656467652e",
					"sequence": 4294967295
				}
			],
			"vout": [
				{
					"value": 50.00000000,
					"n": 0,
					"scriptPubKey": {
						"type": "nonstandard"
					}
				}
			],
			"blockhash": "bf0bc1c9f3a5dcaa07b498707469f3c53104b015a0034b84bf978ae2c8ea9a69",
			"time": 1770499200,
			"blocktime": 1770499200
		},
	},
	genesisBlockStatsByNetwork:{
		"test": {
			"avgfee": 0,
			"avgfeerate": 0,
			"avgtxsize": 0,
			"blockhash": "bf0bc1c9f3a5dcaa07b498707469f3c53104b015a0034b84bf978ae2c8ea9a69",
			"feerate_percentiles": [0, 0, 0, 0, 0],
			"height": 0,
			"ins": 0,
			"maxfee": 0,
			"maxfeerate": 0,
			"maxtxsize": 0,
			"medianfee": 0,
			"mediantime": 1770499200,
			"mediantxsize": 0,
			"minfee": 0,
			"minfeerate": 0,
			"mintxsize": 0,
			"outs": 1,
			"subsidy": 5000000000,
			"swtotal_size": 0,
			"swtotal_weight": 0,
			"swtxs": 0,
			"time": 1770499200,
			"total_out": 0,
			"total_size": 0,
			"total_weight": 0,
			"totalfee": 0,
			"txs": 1,
			"utxo_increase": 1,
			"utxo_size_inc": 117
		},
	},
	testData: {
		txDisplayTestList: {}
	},
	genesisCoinbaseOutputAddressScripthash: null,
	historicalData: [],

	// Freycoin has no exchange rate data yet
	exchangeRateData: null,
	goldExchangeRateData: null,

	blockRewardFunction:function(blockHeight, chain) {
		let halvingBlockInterval = (chain == "regtest" ? 150 : 840000);
		let index = Math.floor(blockHeight / halvingBlockInterval);

		if (index >= blockRewardEras.length) {
			return new Decimal8("0.1"); // tail emission
		}

		return blockRewardEras[index];
	},

	// Prime gap PoW specific: these fields are present in getblock RPC output
	primeGapPoW: true,
	primeGapFields: ["gap", "merit", "shift", "adder", "start_prime", "end_prime", "difficulty"],
};
