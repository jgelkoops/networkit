/*
 * MatchingContracter.cpp
 *
 *  Created on: 30.10.2012
 *      Author: Christian Staudt (christian.staudt@kit.edu)
 */

#include "MatchingContracter.h"

namespace NetworKit {

MatchingContracter::MatchingContracter(const Graph& G, const Matching& M, bool noSelfLoops) : GraphCoarsening(G), M(M), noSelfLoops(noSelfLoops) {

}

void MatchingContracter::run() {
	count n = G.numberOfNodes();
	index z = G.upperNodeIdBound();
	count cn = n - M.size();
	Graph cG(cn, true);

	// compute map: old ID -> new coarse ID
	index idx = 0;
	std::vector<node> mapFineToCoarse(z, none);
	G.forNodes([&](node v) { // TODO: difficult in parallel
		index mate = M.mate(v);
		if ((mate == none) || (v < mate)) {
			// vertex is carried over to the new level
			mapFineToCoarse[v] = idx;
			++idx;
		}
		else {
			// vertex is not carried over, receives ID of mate
			mapFineToCoarse[v] = mapFineToCoarse[mate];
		}
	});

//	for (node v = 0; v < n; ++v) {
//		std::cout << v << " maps to " << mapFineToCoarse[v] << std::endl;
//	}
//	std::cout << "matching size: " << M.matchingSize() << std::endl;

	G.forNodes([&](node v) { // TODO: difficult in parallel
		G.forNeighborsOf(v, [&](node u, edgeweight ew) {
			node cv = mapFineToCoarse[v];
			node cu = mapFineToCoarse[u];
			if (! noSelfLoops || (cv != cu)) {
				cG.increaseWeight(cv, cu, ew);
			}
		});
	});

	Gcoarsed = std::move(cG);
	nodeMapping = std::move(mapFineToCoarse);
}

} /* namespace NetworKit */