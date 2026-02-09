"use strict";

const debug = require("debug");
const debugLog = debug("btcexp:primeGapData");
const debugErrorLog = debug("btcexp:error");

const axios = require("axios");

const allGapsUrl = "https://raw.githubusercontent.com/primegap-list-project/primegap-list-project.github.io/master/_data/allgaps.csv";
const creditsUrl = "https://raw.githubusercontent.com/primegap-list-project/primegap-list-project.github.io/master/_data/credits.csv";

// In-memory cache
let gapMap = new Map();       // gapsize → record object
let creditsMap = new Map();   // discoverer code → full name
let maximalFrontier = [];     // sorted array of {gapsize, primedigits} from ismax=1 records
let lastUpdated = null;
let fetchInProgress = false;

// Retry config
const maxRetries = 3;
const baseRetryDelay = 5000; // 5 seconds

async function fetchWithRetry(url, retries = maxRetries) {
	for (let attempt = 1; attempt <= retries; attempt++) {
		try {
			let response = await axios.get(url, { timeout: 30000 });
			return response.data;
		} catch (err) {
			if (attempt === retries) {
				throw err;
			}
			let delay = baseRetryDelay * Math.pow(2, attempt - 1);
			debugLog(`Fetch attempt ${attempt} failed for ${url}, retrying in ${delay}ms...`);
			await new Promise(resolve => setTimeout(resolve, delay));
		}
	}
}

function parseAllGapsCsv(csvText) {
	let map = new Map();
	let lines = csvText.split("\n");

	for (let line of lines) {
		line = line.trim();
		if (!line || line.startsWith("#")) continue;

		let fields = line.split(",");
		if (fields.length < 10) continue;

		let gapsize = parseInt(fields[0]);
		if (isNaN(gapsize) || gapsize <= 0) continue;

		let record = {
			gapsize: gapsize,
			ismax: parseInt(fields[1]) || 0,
			primecat: fields[2] || "",
			isfirst: fields[3] || "",
			gapcert: fields[4] || "",
			discoverer: fields[5] || "",
			year: parseInt(fields[6]) || 0,
			merit: parseFloat(fields[7]) || 0,
			primedigits: parseInt(fields[8]) || 0,
			startprime: fields[9] || "",
		};

		// Keep the record with the highest merit for each gap size
		let existing = map.get(gapsize);
		if (!existing || record.merit > existing.merit) {
			map.set(gapsize, record);
		}
	}

	return map;
}

function parseCreditsCsv(csvText) {
	let map = new Map();
	let lines = csvText.split("\n");

	for (let line of lines) {
		line = line.trim();
		if (!line || line.startsWith("#")) continue;

		// credits.csv format: code,fullname (first comma splits)
		let commaIdx = line.indexOf(",");
		if (commaIdx < 0) continue;

		let code = line.substring(0, commaIdx).trim();
		let fullName = line.substring(commaIdx + 1).trim();

		if (code && fullName) {
			map.set(code, fullName);
		}
	}

	return map;
}

/**
 * Build a lookup of the largest known gap for each prime digit count.
 * This uses ALL records (not just ismax=1) so we can properly check if a gap
 * exceeds all known gaps for primes of similar or smaller magnitude.
 *
 * The ismax=1 field in allgaps.csv only covers primes up to ~20 digits (exhaustive searches).
 * For larger primes, we need to check against all known records.
 */
function buildMaximalFrontier(gapMapInput) {
	// Build array of (gapsize, primedigits) from ALL records
	let allEntries = [];
	for (let [gapsize, record] of gapMapInput) {
		if (record.primedigits > 0) {
			allEntries.push({ gapsize: gapsize, primedigits: record.primedigits });
		}
	}
	allEntries.sort((a, b) => a.primedigits - b.primedigits || a.gapsize - b.gapsize);

	// Build running maximum: for each digit count, what's the largest gap at or below that count?
	// This lets us answer "is gapSize > all known gaps with primedigits <= X?" in O(log n)
	let frontier = [];
	let runningMax = 0;
	let currentDigits = 0;

	for (let entry of allEntries) {
		if (entry.gapsize > runningMax) {
			runningMax = entry.gapsize;
		}
		// Record the running max at each new digit count
		if (entry.primedigits > currentDigits) {
			// Fill in any skipped digit counts with the previous max
			for (let d = currentDigits + 1; d <= entry.primedigits; d++) {
				frontier.push({ primedigits: d, largestGap: runningMax });
			}
			currentDigits = entry.primedigits;
		} else {
			// Update current entry if this gap is larger
			if (frontier.length > 0) {
				frontier[frontier.length - 1].largestGap = runningMax;
			}
		}
	}

	return frontier;
}

/**
 * Check if a gap would be a new maximal gap.
 * A gap is maximal if gapSize > the largest known gap for ALL primes with
 * primedigits <= our primedigits.
 *
 * This checks against the full database, not just the ismax=1 subset.
 */
function isMaximalGap(gapSize, primedigits) {
	if (maximalFrontier.length === 0) return false;
	if (primedigits <= 0) return false;

	// Find the largest known gap for primes with primedigits <= our primedigits
	// maximalFrontier is indexed by primedigits (1-based)
	let largestKnownGap = 0;
	for (let entry of maximalFrontier) {
		if (entry.primedigits <= primedigits) {
			largestKnownGap = entry.largestGap;
		} else {
			break; // frontier is sorted by primedigits
		}
	}

	// Our gap is maximal only if it exceeds ALL known gaps for primes of our magnitude or smaller
	return gapSize > largestKnownGap;
}

async function initialize() {
	debugLog("Initializing Prime Gap List Project data...");
	await refreshData();
}

async function refreshData() {
	if (fetchInProgress) {
		debugLog("Fetch already in progress, skipping.");
		return;
	}

	fetchInProgress = true;

	try {
		debugLog("Fetching allgaps.csv...");
		let gapsCsv = await fetchWithRetry(allGapsUrl);
		let newGapMap = parseAllGapsCsv(gapsCsv);
		debugLog(`Parsed ${newGapMap.size} prime gap records from Prime Gap List Project`);

		debugLog("Fetching credits.csv...");
		let creditsCsv = await fetchWithRetry(creditsUrl);
		let newCreditsMap = parseCreditsCsv(creditsCsv);
		debugLog(`Parsed ${newCreditsMap.size} discoverer credits`);

		// Build maximal frontier before atomic swap
		let newFrontier = buildMaximalFrontier(newGapMap);
		debugLog(`Built maximal frontier with ${newFrontier.length} maximal gaps`);

		// Atomic swap
		gapMap = newGapMap;
		creditsMap = newCreditsMap;
		maximalFrontier = newFrontier;
		lastUpdated = new Date();

		debugLog(`Fetched ${newGapMap.size} prime gap records from Prime Gap List Project`);

	} catch (err) {
		debugErrorLog(`Failed to fetch Prime Gap List Project data: ${err.message}`);
	} finally {
		fetchInProgress = false;
	}
}

function getAllGaps() {
	return gapMap;
}

function getCredits() {
	return creditsMap;
}

function getLastUpdated() {
	return lastUpdated;
}

/**
 * Cross-reference chain gaps against global records with multi-category badge system.
 *
 * @param {Array} chainGaps - Array of {gap, merit, startPrime, primeBits, ...} objects from the chain
 * @returns {Array} Same array with .badges[] and .globalRecord fields added
 *
 * Badge types:
 *   FIRST_OCCURRENCE  - gap size never recorded in global database
 *   MERIT_RECORD      - our merit exceeds the existing record for this gap size
 *   CERTIFIED_UPGRADE - existing record is only probable (D/d), ours is certified (BPSW)
 *   MAXIMAL           - gap exceeds all known gaps for primes of this magnitude or smaller
 *   KNOWN             - gap size exists in database, no records set
 */
function crossReference(chainGaps) {
	if (!chainGaps || !Array.isArray(chainGaps)) return chainGaps;
	if (gapMap.size === 0) return chainGaps;

	for (let record of chainGaps) {
		let gapSize = record.gap;
		let globalRecord = gapMap.get(gapSize);
		record.badges = [];
		record.globalRecord = globalRecord || null;

		// Estimate prime digits from primeBits or startPrime hex length
		let primedigits = 0;
		if (record.startPrime) {
			// hex digits * log10(16) ≈ hex digits * 1.20412
			let hexLen = record.startPrime.replace(/^0x/, "").length;
			primedigits = Math.floor(hexLen * 1.20412);
		} else if (record.primeBits) {
			// bits * log10(2) ≈ bits * 0.30103
			primedigits = Math.floor(record.primeBits * 0.30103);
		}

		// Category 1: First Known Occurrence
		if (!globalRecord) {
			record.badges.push({
				type: "FIRST_OCCURRENCE",
				label: "First Known Occurrence",
				class: "bg-success"
			});
		}

		// Category 2: Merit Record (our merit beats existing best for this gap size)
		if (globalRecord && record.merit > globalRecord.merit) {
			record.badges.push({
				type: "MERIT_RECORD",
				label: "Merit Record",
				class: "bg-warning text-dark"
			});
		}

		// Category 3: Certified Upgrade
		// If existing record is only probable (gapcert=D or d) and ours improves it
		// Freycoin uses BPSW which qualifies as certified
		if (globalRecord && globalRecord.gapcert !== "C" && record.merit > 0) {
			record.badges.push({
				type: "CERTIFIED_UPGRADE",
				label: "Certified",
				class: "bg-info"
			});
		}

		// Category 4: Maximal Gap
		if (primedigits > 0 && isMaximalGap(gapSize, primedigits)) {
			record.badges.push({
				type: "MAXIMAL",
				label: "Maximal",
				class: "bg-danger"
			});
		}

		// Category 5: Known (no records set) — fallback
		if (record.badges.length === 0 && globalRecord) {
			record.badges.push({
				type: "KNOWN",
				label: "Known",
				class: "bg-secondary"
			});
		}

		// Backwards compatibility: set .badge to the most significant badge type
		if (record.badges.length > 0) {
			let first = record.badges[0];
			if (first.type === "FIRST_OCCURRENCE") {
				record.badge = "NEW_FIRST_KNOWN";
			} else if (first.type === "MERIT_RECORD") {
				record.badge = "HIGHER_MERIT";
			} else {
				record.badge = "EXISTING_RECORD";
			}
		} else {
			record.badge = "EXISTING_RECORD";
		}
	}

	return chainGaps;
}

/**
 * Get gap sizes missing from the global database in a given range.
 * All prime gaps are even (except gap=1 between 2 and 3), so we only check even sizes.
 *
 * @param {number} minSize - Minimum gap size (inclusive)
 * @param {number} maxSize - Maximum gap size (inclusive)
 * @returns {Array} Array of even gap sizes not in the global database
 */
function getMissingGaps(minSize, maxSize) {
	let missing = [];
	// Ensure we start on an even number
	let start = minSize % 2 === 0 ? minSize : minSize + 1;
	for (let g = start; g <= maxSize; g += 2) {
		if (!gapMap.has(g)) {
			missing.push(g);
		}
	}
	return missing;
}

/**
 * Get summary statistics about the global gap database.
 *
 * @returns {Object} { totalGlobal, maximalCount, certifiedCount, probableCount, firstOccurrenceCount, largestGap, highestMerit }
 */
function getRecordSummary() {
	let totalGlobal = gapMap.size;
	let maximalCount = 0;
	let certifiedCount = 0;
	let probableCount = 0;
	let firstOccurrenceCount = 0;
	let largestGap = 0;
	let highestMerit = 0;

	for (let [gapsize, record] of gapMap) {
		if (record.ismax === 1) maximalCount++;
		if (record.gapcert === "C") certifiedCount++;
		if (record.gapcert === "D" || record.gapcert === "d") probableCount++;
		if (record.isfirst === "F") firstOccurrenceCount++;
		if (gapsize > largestGap) largestGap = gapsize;
		if (record.merit > highestMerit) highestMerit = record.merit;
	}

	return {
		totalGlobal,
		maximalCount,
		certifiedCount,
		probableCount,
		firstOccurrenceCount,
		largestGap,
		highestMerit,
	};
}

/**
 * Find the first even gap size not in the global database.
 * Returns the gap size, or null if all are known up to the largest in the DB.
 */
function getFirstMissingGap() {
	if (gapMap.size === 0) return null;
	let maxKnown = 0;
	for (let g of gapMap.keys()) {
		if (g > maxKnown) maxKnown = g;
	}
	for (let g = 2; g <= maxKnown; g += 2) {
		if (!gapMap.has(g)) return g;
	}
	return null;
}

module.exports = {
	initialize,
	refreshData,
	getAllGaps,
	getCredits,
	crossReference,
	getLastUpdated,
	getMissingGaps,
	getRecordSummary,
	isMaximalGap,
	getFirstMissingGap,
};
