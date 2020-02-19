#include "CKBDynamicImpl.h"
#include "CommunityDeathEvent.h"
#include "CommunityBirthEvent.h"
#include "CommunitySplitEvent.h"
#include "CommunityMergeEvent.h"
#include "CustomCommunitySizeDistribution.h"
#include "CustomCommunityMembershipDistribution.h"
#include "PowerlawCommunitySizeDistribution.h"
#include "PowerlawCommunityMembershipDistribution.h"
#include "../../auxiliary/SignalHandling.h"
#include "../../auxiliary/Timer.h"
#include <tlx/unused.hpp>

namespace NetworKit {
	namespace CKBDynamicImpl {
		void CKBDynamicImpl::addEdge(node u, node v, bool nodeJoined) {
			auto e = Community::canonicalEdge(u, v);
			index ts = currentTimeStep;

			if (edgeSharpness < 1 && nodeJoined && currentTimeStep > 0) {
				count offset = edge_sharpness_distribution(urng);
				if (offset < ts) {
					ts -= offset;
				} else {
					ts = 0;
				}
			}

			eventStream.addEdge(ts, e.first, e.second);
		}

		void CKBDynamicImpl::removeEdge(node u, node v, bool nodeLeft) {
			auto e = Community::canonicalEdge(u, v);
			index ts = currentTimeStep;

			if (edgeSharpness < 1 && nodeLeft && currentTimeStep > 0) {
				count offset = edge_sharpness_distribution(urng);
				if (offset + ts < numTimesteps) {
					ts += offset;
				} else {
					ts = numTimesteps;
				}
			}

			eventStream.removeEdge(ts, e.first, e.second);
		}

		void CKBDynamicImpl::addNodeToCommunity(node u, CommunityPtr com) {
			if (com != globalCommunity) {
				if (desiredMemberships[u] == nodeCommunities[u].size()) {
					nodesWithOverassignments.insert(u);
				}
				nodeCommunities[u].insert(com);
				eventStream.nodeJoinsCommunity(currentTimeStep, u, com->getId());
				++currentCommunityMemberships;
			}
		}

		void CKBDynamicImpl::removeNodeFromCommunity(node u, CommunityPtr com) {
			if (com != globalCommunity) {
				nodeCommunities[u].erase(com);
				if (desiredMemberships[u] == nodeCommunities[u].size()) {
					nodesWithOverassignments.erase(u);
				}
				eventStream.nodeLeavesCommunity(currentTimeStep, u, com->getId());
				--currentCommunityMemberships;
			}
		}

		void CKBDynamicImpl::addCommunity(CommunityPtr com) {
			if (com->isAvailable()) {
				availableCommunities.insert(com);
			} else {
				availableCommunities.erase(com);
			}
			communities.insert(com);
		}

		void CKBDynamicImpl::removeCommunity(CommunityPtr com) {
			assert(com->getNumberOfNodes() == 0);
			availableCommunities.erase(com);
			communities.erase(com);
		}

		index CKBDynamicImpl::nextCommunityId() {
			index result = maxCommunityId;
			++maxCommunityId;
			return result;
		}

		index CKBDynamicImpl::drawIndex(index a, index b) {
			std::uniform_int_distribution<index>::param_type p(a, b - 1);
			return uniform_distribution(urng, p);
		}

		index CKBDynamicImpl::drawIndex(index b) {
			return drawIndex(0, b);
		}

		double CKBDynamicImpl::drawBinomial(count numTrials, double probability) {
			std::binomial_distribution<count>::param_type p(numTrials, probability);
			return binomial_distribution(urng, p);
		}

		double CKBDynamicImpl::drawProbability() {
			return random_probability_distribution(urng);
		}

		CKBDynamicImpl::CKBDynamicImpl(const CKBDynamic::param_type &params) :
			urng(Aux::Random::integer()),
			communitySizeSampler(nullptr),
			membershipDistribution(nullptr),
			random_probability_distribution(0, 1),
			edge_sharpness_distribution(params.edgeSharpness),
			maxCommunityId(0),
			sumOfDesiredMemberships(0),
			currentTimeStep(0),
			eventStream(params.numTimesteps + 1),
			n(params.n),
			communityEventProbability(params.communityEventProbability),
			nodeEventProbability(params.nodeEventProbability),
			perturbationProbability(params.perturbationProbability),
			epsilon(params.epsilon),
			edgeSharpness(params.edgeSharpness),
			tEffect(params.tEffect),
			numTimesteps(params.numTimesteps),
			currentCommunityMemberships(0) {

			if (params.G != nullptr && params.C != nullptr) {
				communitySizeSampler.reset(new CustomCommunitySizeDistribution(*params.G, *params.C));
				epsilon = static_cast<CustomCommunitySizeDistribution*>(communitySizeSampler.get())->getEpsilon();
				membershipDistribution.reset(new CustomCommunityMembershipDistribution(*params.G, *params.C));

			} else {
				communitySizeSampler.reset(new PowerlawCommunitySizeDistribution(params.minCommunitySize, params.maxCommunitySize, params.communitySizeExponent, params.intraCommunityEdgeProbability, params.intraCommunityEdgeExponent));
				membershipDistribution.reset(new PowerlawCommunityMembershipDistribution(params.minCommunityMembership, params.maxCommunityMembership, params.communityMembershipExponent));
			}

			double expectedNumberOfCommunities = membershipDistribution->getAverageMemberships() * n / communitySizeSampler->getAverageSize();
			if (expectedNumberOfCommunities < membershipDistribution->getMaximumMemberships()) {
				throw std::runtime_error("Error: Graph impossible to realize, in expectation, there will be " + std::to_string(expectedNumberOfCommunities) + " communities but there may be a node that wants to be part of " + std::to_string(membershipDistribution->getMaximumMemberships()) + " communities.");
			}

		}

		std::vector<GraphEvent> CKBDynamicImpl::getGraphEvents() {
			this->assureFinished();
			return eventStream.getGraphEvents();
		}

		std::vector<CommunityEvent> CKBDynamicImpl::getCommunityEvents() {
			this->assureFinished();
			return eventStream.getCommunityEvents();
		}

		void CKBDynamicImpl::generateNode() {
			node u = desiredMemberships.size();
			desiredMemberships.push_back(membershipDistribution->drawMemberships());
			sumOfDesiredMemberships += desiredMemberships.back();
			nodesAlive.insert(u);
			nodeCommunities.emplace_back();
			globalCommunity->addNode(u);
			eventStream.addNode(currentTimeStep, u);
		}

		void CKBDynamicImpl::eraseNode() {
			node u = nodesAlive.at(drawIndex(nodesAlive.size()));
			sumOfDesiredMemberships -= desiredMemberships[u];
			desiredMemberships[u] = 0;

			while (nodeCommunities[u].size() > 0) {
				CommunityPtr com = *nodeCommunities[u].begin();
				com->removeNode(u);
			}

			assert(nodesAlive.contains(u));
			nodesAlive.erase(u);
			globalCommunity->removeNode(u);
			eventStream.removeNode(currentTimeStep, u);
		}

		void CKBDynamicImpl::run() {
			if (hasRun) throw std::runtime_error("Error, run has already been called");

			Aux::SignalHandler handler;

			// initialization
			globalCommunity = CommunityPtr(new Community(*this));
			globalCommunity->changeEdgeProbability(epsilon);
			communities.erase(globalCommunity);
			availableCommunities.erase(globalCommunity);
			currentTimeStep = 0;

			for (node u = 0; u < n; ++u) {
				generateNode();
			}

			const count initialNumberOfNodes = nodesAlive.size();

			count sumOfDesiredMembers = 0;

			while (sumOfDesiredMembers < sumOfDesiredMemberships) {
				handler.assureRunning();
				count communitySize = communitySizeSampler->drawCommunitySize();;

				CommunityPtr com(new Community(*this));
				com->setDesiredNumberOfNodes(communitySize);
				sumOfDesiredMembers += communitySize;
			}

			assignNodesToCommunities();

			double deathProbability = 0.25, birthProbability = 0.25, splitProbability = 0.25, mergeProbability = 0.25;
			tlx::unused(mergeProbability);

			for (currentTimeStep = 1; currentTimeStep <= numTimesteps; ++currentTimeStep) {
				handler.assureRunning();
				const count numCommunityEvents = drawBinomial(communities.size(), communityEventProbability);

				const count numNodeEvents = drawBinomial(communities.size(), nodeEventProbability);

				INFO("Timestep ", currentTimeStep, " generating ", numCommunityEvents, " community events and ", numNodeEvents, " node events");

				for (count i = 0; i < numCommunityEvents; ++i) {
					{ // adjust event probabilities
						const double x = sumOfDesiredMemberships * 1.0 / sumOfDesiredMembers;
						splitProbability = birthProbability = 0.5 * x / (1 + x);
						mergeProbability = deathProbability = 0.5 - birthProbability;
					}
					handler.assureRunning();
					double r = drawProbability();
					if (r < birthProbability) {
						// generate new community
						count coreSize = communitySizeSampler->getMinSize();
						count targetSize = communitySizeSampler->drawCommunitySize();
						sumOfDesiredMembers += targetSize;
						currentEvents.emplace_back(new CommunityBirthEvent(coreSize, targetSize, tEffect, *this));
					} else if (r < birthProbability + deathProbability) {
						// let a community die
						if (availableCommunities.size() > 0) {
							CommunityPtr com = availableCommunities.at(drawIndex(availableCommunities.size()));
							sumOfDesiredMembers -= com->getDesiredNumberOfNodes();
							count coreSize = communitySizeSampler->getMinSize();
							currentEvents.emplace_back(new CommunityDeathEvent(com, coreSize, tEffect, *this));
							assert(!com->isAvailable());
						} else {
							WARN("No community available for death event.");
						}
					} else if (r < birthProbability + deathProbability + splitProbability) {
						// Split a community
						if (availableCommunities.size() > 0) {
							CommunityPtr com = availableCommunities.at(drawIndex(availableCommunities.size()));
							sumOfDesiredMembers -= com->getDesiredNumberOfNodes();
							count comSizeA = communitySizeSampler->drawCommunitySize();
							sumOfDesiredMembers += comSizeA;
							count comSizeB = communitySizeSampler->drawCommunitySize();
							sumOfDesiredMembers += comSizeB;
							currentEvents.emplace_back(new CommunitySplitEvent(com, comSizeA, comSizeB, tEffect, *this));
							assert(!com->isAvailable());
						} else {
							WARN("No community available for splitting.");
						}
					} else {
						// merge two communities
						if (availableCommunities.size() > 1) {
							index ia = drawIndex(availableCommunities.size());
							index ib = drawIndex(1, availableCommunities.size());
							if (ia == ib) {
								ib = 0;
							}

							CommunityPtr comA = availableCommunities.at(ia);
							sumOfDesiredMembers -= comA->getDesiredNumberOfNodes();
							CommunityPtr comB = availableCommunities.at(ib);
							sumOfDesiredMembers -= comB->getDesiredNumberOfNodes();

							count targetSize = communitySizeSampler->drawCommunitySize();
							sumOfDesiredMembers += targetSize;
							currentEvents.emplace_back(new CommunityMergeEvent(comA, comB, targetSize, tEffect, *this));
							assert(!comA->isAvailable());
							assert(!comB->isAvailable());
						} else {
							WARN("No two communities available for merge.");
						}
					}
				} // generated all new community events

				// generate node events
				const double wantedNodeFraction = initialNumberOfNodes * 1.0 / nodesAlive.size();
				const double nodeBirthProbability = wantedNodeFraction / (1 + wantedNodeFraction);

				// First generate all death events, then all birth events.
				// This ensures that no node that is born in this time step dies again in this time step.
				const count nodesBorn = drawBinomial(numNodeEvents, nodeBirthProbability);

				for (count j = 0; j < (numNodeEvents - nodesBorn); ++j) {
					eraseNode();
				}
				for (count j = 0; j < nodesBorn; ++j) {
					generateNode();
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

					for (CommunityPtr com : communities) {
						handler.assureRunning();
						com->perturbEdges(perturbationProbability);
					}
				}

				assignNodesToCommunities();

				// adjust event probabilities
				{
					INFO("Current memberships: ", currentCommunityMemberships, " desired: ", sumOfDesiredMemberships, ", desired members after events: ", sumOfDesiredMembers," number of communities: ", communities.size(), " available: ", availableCommunities.size(), " active events ", currentEvents.size());
				}
			}

			availableCommunities.clear();
			communities.clear();
			nodeCommunities.clear();
			globalCommunity = nullptr;
			currentEvents.clear();

			eventStream.run();

			hasRun = true;
		}

		void CKBDynamicImpl::assignNodesToCommunities() {
			count totalMissingMembers = 0;

			Aux::Timer timer;
			timer.start();
			std::vector<CommunityPtr> communitiesWithMissingMembers;

			for (const CommunityPtr& com : communities) {
				const count desired = com->getDesiredNumberOfNodes();
				assert(desired >= communitySizeSampler->getMinSize());
				const count actual = com->getNumberOfNodes();
				assert(actual <= desired);

				if (actual < desired) {
					communitiesWithMissingMembers.push_back(com);
					totalMissingMembers += desired - actual;
				}
			}

			if (totalMissingMembers == 0) return;

			count totalMissingMemberships = 0;

			for (node u : nodesAlive) {
				const count desired = desiredMemberships[u];
				const count actual = nodeCommunities[u].size();

				if (desired > actual) {
					totalMissingMemberships += desired - actual;
				}
			}
			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms to collect initial candidates, ", totalMissingMembers, " members to be found, ", totalMissingMemberships, " memberships wanted");

			// If totalMissingMemberships > totalMissingMembers, a) find nodes that have too many memberships and remove those nodes from some of them and b) create empty communities of size 1 that are removed afterwards.
			// If totalMissingMembers > totalMissingMemberships, find nodes to which we can assign additional memberships, i.e., nodes that are not much over their allocated memberships.

			// If we have more node slots free than
			// communities that are missing some members,
			// try to find nodes that got additional
			// members where we can remove some of them.
			// In order to not to disturb merge and split
			// events, we do not take communities that are
			// not available.
			// FIXME: make split and merge events more
			// robust for random node exchanges.
			if (totalMissingMembers < totalMissingMemberships) {
				timer.start();
				for (size_t i = 0; i < nodesWithOverassignments.size() && totalMissingMembers < totalMissingMemberships;) {
					node u = nodesWithOverassignments.sample_item(i);

					assert(nodeCommunities[u].size() > desiredMemberships[u]);

					bool reducedToDesired = false;
					for (size_t ci = 0; ci < nodeCommunities[u].size() && totalMissingMembers < totalMissingMemberships;) {
						const CommunityPtr &com = nodeCommunities[u].sample_item(ci);
						if (com->canRemoveNode()) {
							// If this community had been missing members before, it is already in our list
							if (com->getDesiredNumberOfNodes() <= com->getNumberOfNodes()) {
								communitiesWithMissingMembers.push_back(com);
							}

							com->removeNode(u);
							++totalMissingMembers;

							if (nodeCommunities[u].size() == desiredMemberships[u]) {
								reducedToDesired = true;
								break;
							}
						} else {
							// only increment if we did not remove this community
							// if we removed this community, the item at position ci has been replaced
							// and we can sample again at position ci
							++ci;
						}
					}

					if (!reducedToDesired) {
						// only increment if we did not remove all overassignments
						// if we removed all overassignments, the node has been removed from nodesWithOverAssignments
						// and we can sample again at position i
						++i;
					}
				}
				timer.stop();
				INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms to remove additional nodes from communities, now wanting ", totalMissingMembers, " members");
			}


			timer.start();
			std::vector<std::pair<CommunityPtr, count>> communitiesByDesiredMembers;

			{
				std::vector<count> numCommunitiesWithDesired;

				for (CommunityPtr com : communitiesWithMissingMembers) {
					const count desired = com->getDesiredNumberOfNodes();

					if (desired >= numCommunitiesWithDesired.size()) {
						numCommunitiesWithDesired.resize(desired + 1);
					}

					++numCommunitiesWithDesired[desired];
				}

				count sum = 0;
				for (auto it = numCommunitiesWithDesired.begin(); it != numCommunitiesWithDesired.end(); ++it) {
					const count tmp = *it;
					*it = sum;
					sum += tmp;
				}

				communitiesByDesiredMembers.resize(sum);

				for (CommunityPtr com : communitiesWithMissingMembers) {
					const count desired = com->getDesiredNumberOfNodes();
					const count actual = com->getNumberOfNodes();
					communitiesByDesiredMembers[numCommunitiesWithDesired[desired]] = {com, desired - actual};
					++numCommunitiesWithDesired[desired];
				}
			}


			std::vector<node> nodesByDesiredMemberships(nodesAlive.size(), none);

			{
				std::vector<count> nodesPerDesired;

				for (node u : nodesAlive) {
					const count desired = desiredMemberships[u];
					if (nodesPerDesired.size() <= desired) {
						nodesPerDesired.resize(desired + 1);
					}

					++nodesPerDesired[desired];
				}
				count sum = 0;
				// Reverse prefix sum so the actual order is reversed
				for (auto it = nodesPerDesired.rbegin(); it != nodesPerDesired.rend(); ++it) {
					const count temp = *it;
					*it = sum;
					sum += temp;
				}

				for (node u : nodesAlive) {
					const count desired = desiredMemberships[u];
					nodesByDesiredMemberships[nodesPerDesired[desired]] = u;
					++nodesPerDesired[desired];
				}
			}
			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms to sort nodes and communities");


			timer.start();
			std::vector<count> additionalMembersWanted(nodesByDesiredMemberships.size(), 0);
			count stillMissingMembers = totalMissingMembers;

			// first step: assign only nodes that actually want members
			Aux::SamplingSet<std::pair<node, Community*>, NodeCommunityHash> freshAssignments;
			freshAssignments.reserve(totalMissingMembers);

			// How many communities shall be assigned in the greedy assignment step.
			std::vector<count> freshAssignmentsPerNode(nodesByDesiredMemberships.size(), 0);
			std::vector<node> nodesWantingAdditionalMemberships;

			auto greedilyAssignNode =
				[&](node lu, node u, count numMembers, bool overAssignment) {
					index last_empty = communitiesByDesiredMembers.size();
					count communitiesToFind = numMembers;
					for (index i = 0; i < communitiesByDesiredMembers.size() && communitiesToFind > 0; ++i) {
						// Iterate from back to front
						const index ci = communitiesByDesiredMembers.size() - i - 1;
						const CommunityPtr &com = communitiesByDesiredMembers[ci].first;
						count &missing = communitiesByDesiredMembers[ci].second;
						if (missing > 0 && !com->hasNode(u)) {
							if (freshAssignments.insert({lu, com.get()}) == 1) {
								--missing;
								--stillMissingMembers;
								--communitiesToFind;
								++freshAssignmentsPerNode[lu];
							}
						}

						// Store the last community that is full
						if (missing == 0) {
							last_empty = ci;
						}

						if (communitiesToFind == 0) {
							break;
						}
					}

					// If we found a full community, eliminate any full communities to ensure we do not iterate over them again and again
					// Note that this only iterates over communities that were already touched in the previous loop
					if (last_empty < communitiesByDesiredMembers.size()) {
						// Shift everything to the front
						index wi = last_empty;
						for (index ri = last_empty; ri < communitiesByDesiredMembers.size(); ++ri) {
							if (communitiesByDesiredMembers[ri].second > 0) {
								communitiesByDesiredMembers[wi] = communitiesByDesiredMembers[ri];
								++wi;
							}
						}

						// Remove the remaining entries
						while (communitiesByDesiredMembers.size() > wi) {
							communitiesByDesiredMembers.pop_back();
						}

					}

					if (!overAssignment) {
						additionalMembersWanted[lu] = communitiesToFind;
						for (; communitiesToFind > 0; --communitiesToFind) {
							nodesWantingAdditionalMemberships.push_back(lu);
						}
					}

					assert(freshAssignmentsPerNode[lu] + nodeCommunities[u].size() <= desiredMemberships[u] || overAssignment);
				};


			count nodesAssigned = 0;
			for (node lu = 0; lu < nodesByDesiredMemberships.size(); ++lu) {
				const node u = nodesByDesiredMemberships[lu];
				if (desiredMemberships[u] > nodeCommunities[u].size()) {
					++nodesAssigned;
					greedilyAssignNode(lu, u, desiredMemberships[u] - nodeCommunities[u].size(), false);
				}
			}

			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms for first greedy assignment of ", nodesAssigned, " nodes to ", communitiesWithMissingMembers.size(), " communities, still missing ", stillMissingMembers, " members in ", communitiesByDesiredMembers.size(), " communities.");


			// second step: if communities still want nodes, find out how many memberships are missing and add additional nodes to nodesParticipating.
			timer.start();
			count numRounds = 0;
			double overAssignment = 0;

			count numNodesOverAssigned = 0;
			while (stillMissingMembers > 0) {
				++numRounds;
				overAssignment += std::max(0.01, stillMissingMembers * 1.0 / sumOfDesiredMemberships);

				for (node lu = 0; lu < nodesByDesiredMemberships.size(); ++lu) {
					const node u = nodesByDesiredMemberships[lu];

					// If lu wants additional members this means we couldn't assign it to any community that still wants members
					// So it doesn't make sense to try this node again.
					if (additionalMembersWanted[lu] > 0) continue;

					// Desired memberships with over assignment
					double fdwo = desiredMemberships[u] * (1.0 + overAssignment);
					count dwo = desiredMemberships[u] * (1.0 + overAssignment);
					if (drawProbability() < fdwo - dwo) {
						++dwo;
					}

					if (dwo > nodeCommunities[u].size() + freshAssignmentsPerNode[lu]) {
						++numNodesOverAssigned;
						greedilyAssignNode(lu, u, dwo - nodeCommunities[u].size() - freshAssignmentsPerNode[lu], true);
					}

					if (stillMissingMembers == 0) break;
				}
			}
			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms for over-assignment greedy assignment in ", numRounds, " rounds, tried assigning ", numNodesOverAssigned, " nodes.");


			// third step: randomize community assignments of nodesParticipating, balance assignments

			timer.start();

			std::vector<CommunityPtr> communitiesToShuffle;
			const size_t numFreshAssignments = freshAssignments.size();
			assert(numFreshAssignments == totalMissingMembers);

			count additionalMembershipsUsed = 0;

			for (count round = 0; round < 10 * (totalMissingMembers + nodesWantingAdditionalMemberships.size()); ++round) {
				assert(numFreshAssignments == freshAssignments.size());
				std::array<node, 2> ln;
				std::array<Community*, 2> com {nullptr, nullptr};

				// draw the first node/community pair from freshAssignments so we get a community
				std::tie(ln[0], com[0]) = freshAssignments.at(drawIndex(numFreshAssignments));

				// FIXME: adjust sampling to avoid sampling same index twice

				// draw the second pair possibly with a node that is not yet part of any community but wants one
				// lazy deletion in nodesWantingAdditionalMemberships
				index secondIndex = drawIndex(numFreshAssignments + nodesWantingAdditionalMemberships.size());
				while (true) {
					if (secondIndex < numFreshAssignments) {
						std::tie(ln[1], com[1]) = freshAssignments.at(secondIndex);
						break;
					} else {
						ln[1] = nodesWantingAdditionalMemberships[secondIndex - numFreshAssignments];
						if (additionalMembersWanted[ln[1]] == 0) {
							// oops, this node does not want any more members - delete it and draw again
							nodesWantingAdditionalMemberships[secondIndex - numFreshAssignments] = nodesWantingAdditionalMemberships.back();
							nodesWantingAdditionalMemberships.pop_back();
						} else {
							break;
						}
					}
					secondIndex = drawIndex(numFreshAssignments + nodesWantingAdditionalMemberships.size());
				}

				// Check if we got the same node or community twice
				if (ln[0] == ln[1] || com[0] == com[1]) continue;

				// Check which kind of swap we want to perform
				std::array<node, 2> uv {
					nodesByDesiredMemberships[ln[0]],
					nodesByDesiredMemberships[ln[1]]
				};

				// Calculate the percentage of the desired memberships we would get if we assigned the community
				// to the first/second node.
				std::array<count, 2> assignments;
				std::array<count, 2> desired;
				std::array<double, 2> invertedDesired;
				std::array<double, 2> currentOverAssignment;

				for (index j = 0; j < 2; ++j) {
					assignments[j] = freshAssignmentsPerNode[ln[j]] + nodeCommunities[uv[j]].size();
					desired[j] = desiredMemberships[uv[j]];
					invertedDesired[j] = 1.0 / desired[j];
					currentOverAssignment[j] = assignments[j] * invertedDesired[j];
				}

				assert(assignments[0] > 0);

				if (com[1] == nullptr) {
					assert(currentOverAssignment[1] < 1);

					// try replace node 1 by node 0 if the situation is better for 1 after it loses the community
					if (currentOverAssignment[0] - invertedDesired[0] >= currentOverAssignment[1]) {
						if (com[0]->hasNode(uv[1])) continue;

						if (freshAssignments.insert({ln[1], com[0]})) {
							freshAssignments.erase({ln[0], com[0]});
							// update assignment counters
							--freshAssignmentsPerNode[ln[0]];
							++freshAssignmentsPerNode[ln[1]];
							--additionalMembersWanted[ln[1]];

							// if node 0 already has enough members, remove node 1 from nodesWantingAdditionalMembers
							if (desired[0] + 1 <= assignments[0]) {
								nodesWantingAdditionalMemberships[secondIndex - numFreshAssignments] = nodesWantingAdditionalMemberships.back();
								nodesWantingAdditionalMemberships.pop_back();
							} else { // else add node 0 in place of node 1
								++additionalMembersWanted[ln[0]];
								nodesWantingAdditionalMemberships[secondIndex - numFreshAssignments] = ln[0];
							}

							++additionalMembershipsUsed;
						}
					}
				} else {
					assert(assignments[1] > 0);
					auto replaceNode = [&](index oldNode) -> bool {
						const index targetCom = oldNode;
						const node newNode = 1 - oldNode;
						if (com[targetCom]->hasNode(uv[newNode])) return false;

						if (freshAssignments.insert({ln[newNode], com[targetCom]})) {
							freshAssignments.erase({ln[oldNode], com[targetCom]});
							// update assignment counters
							--freshAssignmentsPerNode[ln[oldNode]];
							++freshAssignmentsPerNode[ln[newNode]];

							if (additionalMembersWanted[ln[newNode]] > 0) {
								--additionalMembersWanted[ln[newNode]];
								// We do not know where it is, the deletion happens lazyly once it reaches 0
							}

							// If the old node has not enough communities anymore, put it into nodesWantingAdditionalMembers
							if (assignments[oldNode] - 1 < desired[oldNode]) {
								++additionalMembersWanted[ln[oldNode]];
								nodesWantingAdditionalMemberships.push_back(ln[oldNode]);
							}
						} else {
							return false;
						}

						return true;
					};

					// We have three options now: 1) assign node 0 to com[1] instead of node 1, 2) assign node 1 to com[0] instead of node 0, 3) switch both nodes,
					// If node 0 has less communities than desired and in its current status has a smaller fraction of desired members than node 1 will have after removing it from com[1]
					// Or if node 0 has enough communities but with the additional community it has still a smaller overassignment than node 1 currently has
					// we move node 0 to com[1] instead of node 1
					if ((assignments[0] < desired[0] && currentOverAssignment[0] < currentOverAssignment[1] - invertedDesired[1])
					    || (assignments[0] >= desired[0] && currentOverAssignment[0] + invertedDesired[0] < currentOverAssignment[1])) {
						if (!replaceNode(1)) continue;
					} else if ((assignments[1] < desired[1] && currentOverAssignment[1] < currentOverAssignment[0] - invertedDesired[0])
					    || (assignments[1] >= desired[1] && currentOverAssignment[1] + invertedDesired[1] < currentOverAssignment[0])) {
						if (!replaceNode(0)) continue;
					} else {
						// swap both communities
						if (com[0]->hasNode(uv[1]) || com[1]->hasNode(uv[0])) continue;
						if (freshAssignments.contains({ln[0], com[1]})) continue;
						if (freshAssignments.insert({ln[1], com[0]})) {
							freshAssignments.erase({ln[0], com[0]});
							freshAssignments.erase({ln[1], com[1]});
							freshAssignments.insert({ln[0], com[1]});
						}
					}
				}
			}
			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms for shuffling ", freshAssignments.size(), " assignments and trying to find ", nodesWantingAdditionalMemberships.size(), " additional assignments, used them ", additionalMembershipsUsed, "times.");

			timer.start();
			// fourth step: Actually assign nodes to communities, thereby eliminating any remaining duplicates.
			for (auto it : freshAssignments) {
				node u = nodesByDesiredMemberships[it.first];
				it.second->addNode(u);
				assert(overAssignment > 0 || nodeCommunities[u].size() <= desiredMemberships[u]);
			}
			timer.stop();
			INFO("Needed ", timer.elapsedMicroseconds() * 1.0 / 1000, "ms to assign ", freshAssignments.size(), " nodes to communities");

			#ifndef NDEBUG
			for (CommunityPtr com : communities) {
				const count desired = com->getDesiredNumberOfNodes();
				assert(desired >= communitySizeSampler->getMinSize());
				const count actual = com->getNumberOfNodes();
				assert(actual == desired);
			}
			#endif
		}
	}
}
