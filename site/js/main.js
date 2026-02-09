(function () {
    "use strict";

    var API_BASE = "https://explorer.freycoin.tech";

    function formatNumber(n) {
        if (typeof n !== "number" || isNaN(n)) return "\u2014";
        return n.toLocaleString("en-US");
    }

    function setText(id, text) {
        var el = document.getElementById(id);
        if (el) el.textContent = text;
    }

    function fetchStats() {
        // Block height from explorer API
        fetch(API_BASE + "/api/blocks/tip/height")
            .then(function (r) { return r.json(); })
            .then(function (data) {
                if (typeof data === "number") {
                    setText("block-height", formatNumber(data));
                } else if (data && typeof data.height === "number") {
                    setText("block-height", formatNumber(data.height));
                }
            })
            .catch(function () {});

        // Mining summary (unique gaps, highest merit, difficulty)
        fetch(API_BASE + "/api/mining/summary")
            .then(function (r) { return r.json(); })
            .then(function (data) {
                if (!data) return;
                if (typeof data.uniqueGaps === "number") {
                    setText("unique-gaps", formatNumber(data.uniqueGaps));
                }
                if (typeof data.highestMerit === "number") {
                    setText("highest-merit", data.highestMerit.toFixed(4));
                }
                if (typeof data.difficulty === "number") {
                    setText("difficulty", data.difficulty.toFixed(4));
                }
            })
            .catch(function () {});
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", fetchStats);
    } else {
        fetchStats();
    }
})();
