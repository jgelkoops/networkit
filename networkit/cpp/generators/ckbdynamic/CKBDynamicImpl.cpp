#include "CKBDynamicImpl.h"
#include "CommunityDeathEvent.h"
#include "CommunityBirthEvent.h"
#include "CommunitySplitEvent.h"
#include "CommunityMergeEvent.h"
#include "PowerlawCommunitySizeDistribution.h"
#include "../../auxiliary/SignalHandling.h"
#include <tlx/unused.hpp>

namespace NetworKit {
	namespace CKBDynamicImpl {
		namespace {
			/**
			 * Returns number of steps you need to wait until the next success (edge) occurs.
			 */
			count get_next_edge_distance(const double log_cp) {
				return static_cast<count>(1 + floor(log(1.0 - Aux::Random::probability()) / log_cp));
			}

		}

		void CKBDynamicImpl::addEdge(node u, node v) {
			auto e = canonicalEdge(u, v);
			auto it = edgesAlive.find(e);

			if (it == edgesAlive.end()) {
				edgesAlive.insert2(e, 1);
				graphEvents.emplace_back(GraphEvent::EDGE_ADDITION, e.first, e.second);
			} else {
				edgesAlive[e] += 1;
			}
		}

		void CKBDynamicImpl::removeEdge(node u, node v) {
			auto e = canonicalEdge(u, v);
			auto it = edgesAlive.find(e);
			if (it == edgesAlive.end()) {
				throw std::runtime_error("Error, removing edge that does not exist");
			}

			if (it->second > 1) {
				edgesAlive[e] -= 1;
			} else {
				edgesAlive.erase(it);
				graphEvents.emplace_back(GraphEvent::EDGE_REMOVAL, e.first, e.second);
			}
		}

		void CKBDynamicImpl::addNodeToCommunity(node u, CommunityPtr com) {
			if (com != globalCommunity) {
				nodeCommunities[u].insert(com);
				communityEvents.emplace_back(CommunityEvent::NODE_JOINS_COMMUNITY, u, com->getId());
				++currentCommunityMemberships;
				communityNodeSampler.assignCommunity(u);

				if (com->isAvailable()) {
					if (com->getNumberOfNodes() == 2*communitySizeSampler->getMinSize()) {
						splittableCommunities.insert(com);
					}

					if (com->getNumberOfNodes() > (communitySizeSampler->getMaxSize() - communitySizeSampler->getMinSize())) {
						mergeableCommunities.erase(com);
					}
				}
			}
		}

		void CKBDynamicImpl::removeNodeFromCommunity(node u, CommunityPtr com) {
			if (com != globalCommunity) {
				nodeCommunities[u].erase(com);
				communityEvents.emplace_back(CommunityEvent::NODE_LEAVES_COMMUNITY, u, com->getId());
				communityNodeSampler.leaveCommunity(u);
				--currentCommunityMemberships;

				if (com->isAvailable()) {
					if (com->getNumberOfNodes() < 2*communitySizeSampler->getMinSize()) {
						splittableCommunities.erase(com);
					}

					if (com->getNumberOfNodes() <= communitySizeSampler->getMaxSize()) {
						mergeableCommunities.insert(com);
					}
				}
			}
		}

		void CKBDynamicImpl::addCommunity(CommunityPtr com) {
			// FIXME: do not add global community? but impossible because addCommunity is called in constructor...
			if (com->isAvailable()) {
				// If community is too small, remove community again!!
				if (com->getNumberOfNodes() < communitySizeSampler->getMinSize()) {
					INFO("community has only ", com->getNumberOfNodes(), " nodes, destroying.");
					currentEvents.emplace_back(new CommunityDeathEvent(com, 0, 1, *this));
				} else {
					availableCommunities.insert(com);
					if (com->getNumberOfNodes() >= 2*communitySizeSampler->getMinSize()) {
						splittableCommunities.insert(com);
					}

					if (com->getNumberOfNodes() <= communitySizeSampler->getMaxSize() - communitySizeSampler->getMinSize()) {
						mergeableCommunities.insert(com);
					}
				}
			} else {
				availableCommunities.erase(com);
				splittableCommunities.erase(com);
				mergeableCommunities.erase(com);
			}
			communities.insert(com);
		}

		void CKBDynamicImpl::removeCommunity(CommunityPtr com) {
			availableCommunities.erase(com);
			communities.erase(com);
			mergeableCommunities.erase(com);
			splittableCommunities.erase(com);
		}

		index CKBDynamicImpl::nextCommunityId() {
			index result = maxCommunityId;
			++maxCommunityId;
			return result;
		}

		CKBDynamicImpl::CKBDynamicImpl(const CKBDynamic::param_type &params) :
			communityNodeSampler(0, params.minCommunityMembership, params.maxCommunityMembership, params.communityMembershipExponent),
			communitySizeSampler(new PowerlawCommunitySizeDistribution(params.minCommunitySize, params.maxCommunitySize, params.communitySizeExponent, params.intraCommunityEdgeProbability, params.intraCommunityEdgeExponent, params.minSplitRatio)),
			n(params.n),
			communityEventProbability(params.communityEventProbability),
			nodeEventProbability(params.nodeEventProbability),
			perturbationProbability(params.perturbationProbability),
			epsilon(params.epsilon),
			numTimesteps(params.numTimesteps),
			currentCommunityMemberships(0) {
		}

		std::vector<GraphEvent> CKBDynamicImpl::getGraphEvents() const {
			this->assureFinished();
			return std::move(graphEvents);
		}

		std::vector<CommunityEvent> CKBDynamicImpl::getCommunityEvents() const {
			this->assureFinished();
			return std::move(communityEvents);
		}

		void CKBDynamicImpl::generateNode() {
			node u = communityNodeSampler.addNode();
			for (node v = u; v < communityNodeSampler.getNumberOfNodes(); ++v) {
				nodesAlive.insert(v);
				nodeCommunities.emplace_back();
				globalCommunity->addNode(v);
			}
		}

		void CKBDynamicImpl::eraseNode() {
			node u = nodesAlive.at(Aux::Random::index(nodesAlive.size()));
			while (nodeCommunities[u].size() > 0) {
				CommunityPtr com = *nodeCommunities[u].begin();
				com->removeNode(u);
				// if a community becomes too small, erase it
				if (com->isAvailable() && com->getNumberOfNodes() < communitySizeSampler->getMinSize()) {
					INFO("Available community has only ", com->getNumberOfNodes(), " nodes, destroying.");
					currentEvents.emplace_back(new CommunityDeathEvent(com, 0, 1, *this));
				}
			}

			nodesAlive.erase(u);
			communityNodeSampler.removeNode(u);
			globalCommunity->removeNode(u);
		}

		count CKBDynamicImpl::sampleNumSteps() const {
			return Aux::Random::integer(5, 15);
		}

		void CKBDynamicImpl::run() {
			if (hasRun) throw std::runtime_error("Error, run has already been called");

			Aux::SignalHandler handler;

			// initialization
			globalCommunity = CommunityPtr(new Community(epsilon, *this));

			for (node u = 0; u < n; ++u) {
				generateNode();
			}

			const count initialNumberOfNodes = nodesAlive.size();

			while (currentCommunityMemberships < communityNodeSampler.getSumOfDesiredMemberships()) {
				handler.assureRunning();
				count communitySize;
				double edgeProbability;
				std::tie(communitySize, edgeProbability) = communitySizeSampler->drawCommunity();
				std::vector<node> comNodes(communityNodeSampler.birthCommunityNodes(communitySize));
				CommunityPtr com(new Community(edgeProbability, *this));
				for (node u : comNodes) {
					com->addNode(u);
				}
				com->setAvailable(true);
			}

			std::binomial_distribution<count> numEventDistribution;
			double deathProbability = 0.25, birthProbability = 0.25, splitProbability = 0.25, mergeProbability = 0.25;
			tlx::unused(mergeProbability);

			for (count timestep = 0; timestep < numTimesteps; ++timestep) {
				handler.assureRunning();
				graphEvents.emplace_back(GraphEvent::TIME_STEP);
				communityEvents.emplace_back(CommunityEvent::TIME_STEP);

				numEventDistribution.param(std::binomial_distribution<count>::param_type(communities.size(), communityEventProbability));
				const count numCommunityEvents = numEventDistribution(Aux::Random::getURNG());

				numEventDistribution.param(std::binomial_distribution<count>::param_type(nodesAlive.size(), nodeEventProbability));
				const count numNodeEvents = numEventDistribution(Aux::Random::getURNG());

				INFO("Timestep ", timestep, " generating ", numCommunityEvents, " community events and ", numNodeEvents, " node events");

				for (count i = 0; i < numCommunityEvents; ++i) {
					handler.assureRunning();
					count numSteps = sampleNumSteps();
					double r = Aux::Random::real();
					if (r < birthProbability) {
						// generate new community
						currentEvents.emplace_back(new CommunityBirthEvent(numSteps, *this));
					} else if (r < birthProbability + deathProbability) {
						// let a community die
						if (availableCommunities.size() > 0) {
							CommunityPtr com = availableCommunities.at(Aux::Random::index(availableCommunities.size()));
							count coreSize = std::max<count>(0.1 * com->getNumberOfNodes(), communitySizeSampler->getMinSize());
							currentEvents.emplace_back(new CommunityDeathEvent(com, coreSize, numSteps, *this));
							assert(!com->isAvailable());
						} else {
							WARN("No community available for death event.");
						}
					} else if (r < birthProbability + deathProbability + splitProbability) {
						// Split a community
						if (splittableCommunities.size() > 0) {
							CommunityPtr com = splittableCommunities.at(Aux::Random::index(splittableCommunities.size()));
							auto comSizeProb = communitySizeSampler->splitCommunity(com->getNumberOfNodes(), com->getEdgeProbability());
							currentEvents.emplace_back(new CommunitySplitEvent(com, comSizeProb.first.first, comSizeProb.first.second, comSizeProb.second.first, comSizeProb.second.second, numSteps, *this));
							assert(!com->isAvailable());
						} else {
							WARN("No community available for splitting.");
						}
					} else {
						// merge two communities
						if (mergeableCommunities.size() > 1) {
							CommunityPtr comA, comB;
							bool found = false;
							for (count j = 0; j < 20; ++j) {
								comA = mergeableCommunities.at(Aux::Random::index(mergeableCommunities.size()));
								comB = mergeableCommunities.at(Aux::Random::index(mergeableCommunities.size()));
								if (comA != comB && comA->getNumberOfNodes() + comB->getNumberOfNodes() < communitySizeSampler->getMaxSize()) {
									found = true;
									break;
								}
							}

							if (found) {
								currentEvents.emplace_back(new CommunityMergeEvent(comA, comB, numSteps, *this));
								assert(!comA->isAvailable());
								assert(!comB->isAvailable());
							} else {
								WARN("In 20 trials, no two communities found to merge.");
							}
						} else {
							WARN("No two communities available for merge.");
						}
					}
				} // generated all new community events

				// generate node events
				const double wantedNodeFraction = initialNumberOfNodes * 1.0 / nodesAlive.size();
				const double nodeBirthProbability = wantedNodeFraction / (1 + wantedNodeFraction);
				for (count j = 0; j < numNodeEvents; ++j) {
					if (Aux::Random::real() < nodeBirthProbability) {
						generateNode();
					} else {
						eraseNode();
					}
				}

				// Trigger all current events
				for (size_t e = 0; e < currentEvents.size();) {
					handler.assureRunning();
					currentEvents[e]->nextStep();

					if (!currentEvents[e]->isActive()) {
						std::swap(currentEvents[e], currentEvents.back());
						currentEvents.pop_back();
					} else {
						++e;
					}
				}

				// edge perturbations
				if (perturbationProbability > 0) {
					globalCommunity->perturbEdges(perturbationProbability);

					const double sqrtPerturbationProbability = std::sqrt(perturbationProbability);

					const double log_perturb = std::log(1.0 - sqrtPerturbationProbability);

					for (count ci = get_next_edge_distance(log_perturb) - 1; ci < communities.size(); ci += get_next_edge_distance(log_perturb)) {
						handler.assureRunning();
						communities.at(ci)->perturbEdges(sqrtPerturbationProbability);
					}
				}

				// adjust event probabilities
				{
					const double x = communityNodeSampler.getSumOfDesiredMemberships() * 1.0 / currentCommunityMemberships;
					birthProbability = 0.5 * x / (1 + x);
					deathProbability = 0.5 - birthProbability;
					INFO("At timestep ", timestep, " adjusting birth probability to ", birthProbability, " and death probability to ", deathProbability);
					INFO("Current memberships: ", currentCommunityMemberships, " desired: ", communityNodeSampler.getSumOfDesiredMemberships(), " number of communities: ", communities.size(), " available: ", availableCommunities.size(), " active events ", currentEvents.size());
					INFO("Current nodes ", nodesAlive.size(), " current edges: ", edgesAlive.size(), " total graph events ", graphEvents.size(), " total community events ", communityEvents.size());
				}

			}

			graphEvents.emplace_back(GraphEvent::TIME_STEP);
			communityEvents.emplace_back(CommunityEvent::TIME_STEP);

			hasRun = true;
		}

	}
}
