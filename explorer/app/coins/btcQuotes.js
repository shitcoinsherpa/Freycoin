"use strict";

module.exports = {
	items: [
		{
			text: "Jonnie Frey 1989-2017. Every proof of work advances mathematical knowledge.",
			speaker: "Freycoin Genesis Block",
			url: "./block-height/0",
			date: "2025-06-08",
			context: "embedded in the Freycoin genesis block coinbase"
		},

		// ═══════════════════════════════════════════════
		// JONNIE FREY (j0nn9) — Creator of Gapcoin
		// ═══════════════════════════════════════════════

		{
			text: "Instead of burning electricity for its own sake, Gapcoins Proof of Work function actually does useful work by searching for large prime gaps.",
			speaker: "Jonnie Frey",
			date: "2014-10",
			url: "https://gapcoin.org/",
			context: "from the Gapcoin website"
		},
		{
			text: "Researches about prime gaps could not only lead to new breakthroughs in the bounded gap, it may also help proving the Twin Prime Conjecture and maybe even the millennium problem, the Riemann hypothesis.",
			speaker: "Jonnie Frey",
			date: "2014-10",
			url: "https://gapcoin.org/",
			context: "from the Gapcoin website"
		},
		{
			text: "The merit is the ratio of the size of a prime gap in comparison to the average size of a prime gap in the proximity of p. To find a gap with at least merit m, the average calculation amount is e^m primes.",
			speaker: "Jonnie Frey",
			date: "2014-12-14",
			url: "https://bitcointalk.org/index.php?topic=822498.0",
			context: "BitcoinTalk technical explanation of Gapcoin's PoW"
		},
		{
			text: "The average length of a prime gap with the starting prime p, is log(p), which means that the average prime gap size increases with larger primes.",
			speaker: "Jonnie Frey",
			date: "2014-11-19",
			url: "https://gapcoin-project.github.io/j0nn9-technotes/",
			context: "BitcoinTalk technical notes on prime gap mining"
		},
		{
			text: "I just want to inform you that Gapcoin just broke 544 records of first known occurrence prime gaps.",
			speaker: "Jonnie Frey",
			date: "2014-12-16",
			url: "https://bitcointalk.org/index.php?topic=822498.0",
			context: "announcing Gapcoin's contribution to mathematical records"
		},
		{
			text: "Even if we are currently a few of steps away from the world record of the highest merit primegap, I'm pretty sure Gapcoin can break some more records on those lists.",
			speaker: "Jonnie Frey",
			date: "2014-12-16",
			url: "https://bitcointalk.org/index.php?topic=822498.0"
		},
		{
			text: "Hi there, first of all I'd like to thank you all for your great support over the years. Second I want to confirm that Gapcoin indeed broke the World Record for the largest known prime gap of maximum known merit.",
			speaker: "Jonnie Frey",
			date: "2018-03-23",
			url: "https://bitcointalk.org/index.php?topic=822498.1640",
			context: "his final known public post, confirming the world record"
		},
		{
			text: "From my point of view, the miner is probable at the performance limit, but maybe someone else does have some smart Ideas.",
			speaker: "Jonnie Frey",
			date: "2015-03-13",
			url: "https://gapcoin-project.github.io/j0nn9-technotes/",
			context: "BitcoinTalk post on mining optimization"
		},
		{
			text: "About the speed: I managed to get around 1,200,000 pps with a single AMD R9 280, that's a 6x speed increase to the previous GPU-Miner. I could also reduce the memory load. Only the Sieve still runs on the CPU.",
			speaker: "Jonnie Frey",
			date: "2014-12-05",
			url: "https://gapcoin-project.github.io/j0nn9-technotes/",
			context: "announcing the improved GPU miner for prime gap searching"
		},

		// ═══════════════════════════════════════════════
		// SATOSHI NAKAMOTO — Proof-of-Work and Computation
		// ═══════════════════════════════════════════════

		{
			text: "Proof-of-work has the nice property that it can be relayed through untrusted middlemen.",
			speaker: "Satoshi Nakamoto",
			date: "2010-08-07",
			url: "https://satoshi.nakamotoinstitute.org/posts/bitcointalk/327/"
		},
		{
			text: "It doesn't matter who tells you a longest chain, the proof-of-work speaks for itself.",
			speaker: "Satoshi Nakamoto",
			date: "2010-08-07",
			url: "https://satoshi.nakamotoinstitute.org/posts/bitcointalk/327/"
		},
		{
			text: "The proof-of-work chain is the solution to the synchronisation problem, and to knowing what the globally shared view is without having to trust anyone.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-09",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/6/"
		},
		{
			text: "The proof-of-work involves scanning for a value that when hashed, such as with SHA-256, the hash begins with a number of zero bits. The average work required is exponential in the number of zero bits required and can be verified by executing a single hash.",
			speaker: "Satoshi Nakamoto",
			date: "2008-10-31",
			url: "https://bitcoin.org/bitcoin.pdf",
			context: "from the whitepaper, Section 4: Proof-of-Work"
		},
		{
			text: "Once the CPU effort has been expended to make it satisfy the proof-of-work, the block cannot be changed without redoing the work.",
			speaker: "Satoshi Nakamoto",
			date: "2008-10-31",
			url: "https://bitcoin.org/bitcoin.pdf",
			context: "from the whitepaper, Section 4: Proof-of-Work"
		},
		{
			text: "Proof-of-work is essentially one-CPU-one-vote. The majority decision is represented by the longest chain, which has the greatest proof-of-work effort invested in it.",
			speaker: "Satoshi Nakamoto",
			date: "2008-10-31",
			url: "https://bitcoin.org/bitcoin.pdf",
			context: "from the whitepaper, Section 4: Proof-of-Work"
		},
		{
			text: "The credential that establishes someone as real is the ability to supply CPU power.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-15",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/14/"
		},
		{
			text: "The proof-of-work chain is a solution to the Byzantine Generals' Problem.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-13",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/11/"
		},
		{
			text: "Every general, just by verifying the difficulty of the proof-of-work chain, can estimate how much parallel CPU power per hour was expended on it and see that it must have required the majority of the computers to produce that much proof-of-work in the allotted time. They had to all have seen it because the proof-of-work is proof that they worked on it.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-13",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/11/",
			context: "explaining the Byzantine Generals' analogy"
		},
		{
			text: "I had to write all the code before I could convince myself that I could solve every problem, then I wrote the paper.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-09",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/6/"
		},
		{
			text: "The proof-of-work is a Hashcash style SHA-256 collision finding. It's a memoryless process where you do millions of hashes a second, with a small chance of finding one each time. Anyone's chance of finding a solution at any time is proportional to their CPU power.",
			speaker: "Satoshi Nakamoto",
			date: "2008-11-15",
			url: "https://satoshi.nakamotoinstitute.org/emails/cryptography/14/"
		},

		// ═══════════════════════════════════════════════
		// HAL FINNEY — Cryptography, Computation, Math
		// ═══════════════════════════════════════════════

		{
			text: "I like the idea of basing security on the assumption that the CPU power of honest participants outweighs that of the attacker. It is a very modern notion that exploits the power of the long tail.",
			speaker: "Hal Finney",
			date: "2008-11-08",
			url: "https://www.metzdowd.com/pipermail/cryptography/2008-November/014981.html",
			context: "responding to Satoshi's whitepaper on the cryptography mailing list"
		},
		{
			text: "The computer can be used as a tool to liberate and protect people, rather than to control them.",
			speaker: "Hal Finney",
			date: "1992-11-15",
			url: "https://nakamotoinstitute.org/library/why-remailers-i/",
			context: "from \"Why Remailers I\" on the Cypherpunks mailing list"
		},
		{
			text: "Cryptography can make possible a world in which people have control over information about themselves, not because government has granted them that control, but because only they possess the cryptographic keys to reveal that information.",
			speaker: "Hal Finney",
			date: "1993",
			url: "https://nakamotoinstitute.org/library/protecting-privacy-with-electronic-cash/",
			context: "from \"Protecting Privacy with Electronic Cash\" in Extropy magazine"
		},
		{
			text: "The work we are doing here, broadly speaking, is dedicated to this goal of making Big Brother obsolete. It's important work. If things work out well, we may be able to look back and see that it was the most important work we have ever done.",
			speaker: "Hal Finney",
			date: "1992-11-15",
			url: "https://nakamotoinstitute.org/library/why-remailers-i/",
			context: "from \"Why Remailers I\" on the Cypherpunks mailing list"
		},
		{
			text: "Running bitcoin",
			speaker: "Hal Finney",
			date: "2009-01-11",
			url: "https://twitter.com/halfin/status/1110302988",
			context: "the tweet heard around the world — possibly the first person after Satoshi to run the software"
		},
		{
			text: "I've noticed that cryptographic graybeards (I was in my mid 50's) tend to get cynical. I was more idealistic; I have always loved crypto, the mystery and the paradox of it.",
			speaker: "Hal Finney",
			date: "2013-03-19",
			url: "https://bitcointalk.org/index.php?topic=155054.0",
			context: "from \"Bitcoin and me\" — his last major public post"
		},
		{
			text: "I want to prove to you that I know a message that hashes to a given hash value using the SHA-1 hash. I don't want to reveal anything about the message to you. It's a zero-knowledge proof, and I've written a program to do this that I'll tell you about.",
			speaker: "Hal Finney",
			date: "1998-08-26",
			url: "https://cointelegraph.com/news/bitcoin-pioneer-hal-finney-talks-zero-knowledge-proofs-newly-surfaced-video",
			context: "presentation at the Crypto '98 conference, UC Santa Barbara"
		},
		{
			text: "I recently discovered that I can even write code. It's very slow, probably 50 times slower than I was before. But I still love programming and it gives me goals.",
			speaker: "Hal Finney",
			date: "2013-03-19",
			url: "https://bitcointalk.org/index.php?topic=155054.0",
			context: "from \"Bitcoin and me\" — written while living with ALS"
		},
		{
			text: "Digital cash has the property that when exponentiated it leads to a number most of whose bits are fixed but which has a small number of varying bits.",
			speaker: "Hal Finney",
			date: "1994-03-16",
			url: "https://nakamotoinstitute.org/library/the-beauty-of-ecash/",
			context: "from \"The Beauty of ECash\" — exploring the aesthetics of cryptographic mathematics"
		},

		// ═══════════════════════════════════════════════
		// MATHEMATICIANS — Primes and Number Theory
		// ═══════════════════════════════════════════════

		{
			text: "Mathematics is the queen of the sciences and number theory is the queen of mathematics.",
			speaker: "Carl Friedrich Gauss",
			url: "https://en.wikiquote.org/wiki/Carl_Friedrich_Gauss"
		},
		{
			text: "The problem of distinguishing prime numbers from composite numbers and of resolving the latter into their prime factors is known to be one of the most important and useful in arithmetic.",
			speaker: "Carl Friedrich Gauss",
			date: "1801",
			url: "https://en.wikipedia.org/wiki/Disquisitiones_Arithmeticae",
			context: "from Disquisitiones Arithmeticae"
		},
		{
			text: "God may not play dice with the universe, but something strange is going on with the prime numbers.",
			speaker: "Paul Erdős",
			url: "https://en.wikiquote.org/wiki/Paul_Erd%C5%91s",
			context: "attributed, paraphrasing Einstein"
		},
		{
			text: "A mathematician is a device for turning coffee into theorems.",
			speaker: "Alfréd Rényi",
			url: "https://en.wikiquote.org/wiki/Alfr%C3%A9d_R%C3%A9nyi",
			context: "frequently attributed to Paul Erdős"
		},
		{
			text: "The primes are the raw material out of which we have to build arithmetic, and Euclid's theorem assures us that we have plenty of material for the task.",
			speaker: "G.H. Hardy",
			date: "1940",
			url: "https://en.wikipedia.org/wiki/A_Mathematician%27s_Apology",
			context: "from A Mathematician's Apology"
		},
		{
			text: "No one has yet discovered any warlike purpose to be served by the theory of numbers or relativity, and it seems very unlikely that anyone will do so for many years.",
			speaker: "G.H. Hardy",
			date: "1940",
			url: "https://en.wikipedia.org/wiki/A_Mathematician%27s_Apology",
			context: "from A Mathematician's Apology (written before the atomic bomb and modern cryptography proved him spectacularly wrong)"
		},
		{
			text: "The scientist does not study nature because it is useful; he studies it because he delights in it, and he delights in it because it is beautiful.",
			speaker: "Henri Poincaré",
			date: "1908",
			url: "https://en.wikiquote.org/wiki/Henri_Poincar%C3%A9",
			context: "from Science and Method"
		},
		{
			text: "Prime numbers are what is left when you have taken all the patterns away. I think prime numbers are like life.",
			speaker: "Mark Haddon",
			date: "2003",
			url: "https://en.wikipedia.org/wiki/The_Curious_Incident_of_the_Dog_in_the_Night-Time",
			context: "from The Curious Incident of the Dog in the Night-Time"
		},
		{
			text: "Pure mathematics is, in its way, the poetry of logical ideas.",
			speaker: "Albert Einstein",
			date: "1935",
			url: "https://en.wikiquote.org/wiki/Albert_Einstein"
		},
		{
			text: "Young man, in mathematics you don't understand things. You just get used to them.",
			speaker: "John von Neumann",
			url: "https://en.wikiquote.org/wiki/John_von_Neumann"
		},
		{
			text: "Euclid alone has looked on Beauty bare.",
			speaker: "Edna St. Vincent Millay",
			date: "1923",
			url: "https://www.poetryfoundation.org/poems/148579/euclid-alone-has-looked-on-beauty-bare"
		},
		{
			text: "It is evident that the primes are randomly distributed but, unfortunately, we don't know what 'random' means.",
			speaker: "R.C. Vaughan",
			url: "https://en.wikipedia.org/wiki/Robert_Charles_Vaughan_(mathematician)",
			context: "lecture at the University of Pennsylvania"
		},
		{
			text: "Mathematics is not a careful march down a well-cleared highway, but a journey into a strange wilderness, where the explorers often get lost.",
			speaker: "W.S. Anglin",
			url: "https://en.wikipedia.org/wiki/W.S._Anglin",
			context: "from Mathematics and History"
		},
		{
			text: "We may — paraphrasing the great mathematician David Hilbert — say about the Riemann Hypothesis what he said about a famous open problem of his time: There it lies. Solve it. Take it away.",
			speaker: "Enrico Bombieri",
			date: "2000",
			url: "https://www.claymath.org/millennium/riemann-hypothesis/",
			context: "from the Clay Mathematics Institute Millennium Prize description"
		},
		{
			text: "Despite their simple definition and role as the building blocks of the natural numbers, the prime numbers grow like weeds among the natural numbers, seeming to obey no other law than that of chance, and nobody can predict where the next one will sprout. The second fact is even more astonishing, for it states just the opposite: that the prime numbers exhibit stunning regularity, that there are laws governing their behavior, and that they obey these laws with almost military precision.",
			speaker: "Don Zagier",
			date: "1977",
			url: "https://en.wikipedia.org/wiki/Don_Zagier",
			context: "from \"The first 50 million prime numbers,\" Mathematical Intelligencer"
		},
		{
			text: "Mathematics, rightly viewed, possesses not only truth, but supreme beauty — a beauty cold and austere, like that of sculpture.",
			speaker: "Bertrand Russell",
			date: "1903",
			url: "https://en.wikiquote.org/wiki/Bertrand_Russell",
			context: "from \"The Study of Mathematics\" in Mysticism and Logic"
		},

		// ═══════════════════════════════════════════════
		// PROOF-OF-WORK PHILOSOPHY
		// ═══════════════════════════════════════════════

		{
			text: "There is no link between the informational realm and the physical realm. Proof of work is the only thing that creates this link in a probabilistic fashion, because it creates information that speaks for itself.",
			speaker: "Gigi",
			date: "2022-05-12",
			url: "https://youtu.be/w6VdgEz0NV4?t=1113"
		},
		{
			text: "By attaching energy to a block, we give it 'form', allowing it to have real weight & consequences in the physical world.",
			speaker: "Hugo Nguyen",
			date: "2018-02-10",
			url: "https://bitcointechtalk.com/the-anatomy-of-proof-of-work-98c85b6f6667"
		},
	]
};
