#include "search/features_layer_path_finder.hpp"

#include "search/cancel_exception.hpp"
#include "search/features_layer_matcher.hpp"
#include "search/house_numbers_matcher.hpp"

#include "indexer/features_vector.hpp"

#include "base/cancellable.hpp"
#include "base/stl_helpers.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>

using namespace std;

namespace search
{
// static
FeaturesLayerPathFinder::Mode FeaturesLayerPathFinder::m_mode = MODE_AUTO;

namespace
{
using ParentGraph = deque<unordered_map<uint32_t, uint32_t>>;

// This function tries to estimate amount of work needed to perform an
// intersection pass on a sequence of layers.
template <typename TIt>
uint64_t CalcPassCost(TIt begin, TIt end)
{
  uint64_t cost = 0;

  if (begin == end)
    return cost;

  uint64_t reachable = max((*begin)->m_sortedFeatures->size(), size_t(1));
  for (++begin; begin != end; ++begin)
  {
    uint64_t const layer = max((*begin)->m_sortedFeatures->size(), size_t(1));
    cost += layer * reachable;
    reachable = min(reachable, layer);
  }
  return cost;
}

uint64_t CalcTopDownPassCost(vector<FeaturesLayer const *> const & layers)
{
  return CalcPassCost(layers.rbegin(), layers.rend());
}

uint64_t CalcBottomUpPassCost(vector<FeaturesLayer const *> const & layers)
{
  return CalcPassCost(layers.begin(), layers.end());
}

bool GetPath(uint32_t id, vector<FeaturesLayer const *> const & layers, ParentGraph const & parent,
             IntersectionResult & result)
{
  result.Clear();
  if (layers.size() != parent.size() + 1)
    return false;

  size_t level = 0;
  for (auto parentGraphLayer = parent.crbegin(); parentGraphLayer != parent.crend();
       ++parentGraphLayer, ++level)
  {
    result.Set(layers[level]->m_type, id);
    auto const it = parentGraphLayer->find(id);
    if (it == parentGraphLayer->cend())
      return false;
    id = it->second;
  }
  result.Set(layers[level]->m_type, id);
  return true;
}

bool MayHaveDelayedFeatures(FeaturesLayer const & layer)
{
  return layer.m_type == Model::TYPE_BUILDING &&
         house_numbers::LooksLikeHouseNumber(layer.m_subQuery, layer.m_lastTokenIsPrefix);
}
}  // namespace

FeaturesLayerPathFinder::FeaturesLayerPathFinder(::base::Cancellable const & cancellable)
  : m_cancellable(cancellable)
{
}

void FeaturesLayerPathFinder::FindReachableVertices(FeaturesLayerMatcher & matcher,
                                                    vector<FeaturesLayer const *> const & layers,
                                                    vector<IntersectionResult> & results)
{
  if (layers.empty())
    return;

  switch (m_mode)
  {
  case MODE_AUTO:
  {
    uint64_t const topDownCost = CalcTopDownPassCost(layers);
    uint64_t const bottomUpCost = CalcBottomUpPassCost(layers);

    if (bottomUpCost < topDownCost)
      FindReachableVerticesBottomUp(matcher, layers, results);
    else
      FindReachableVerticesTopDown(matcher, layers, results);
  }
  break;
  case MODE_BOTTOM_UP: FindReachableVerticesBottomUp(matcher, layers, results); break;
  case MODE_TOP_DOWN: FindReachableVerticesTopDown(matcher, layers, results); break;
  }
}

void FeaturesLayerPathFinder::FindReachableVerticesTopDown(
    FeaturesLayerMatcher & matcher, vector<FeaturesLayer const *> const & layers,
    vector<IntersectionResult> & results)
{
  ASSERT(!layers.empty(), ());

  vector<uint32_t> reachable = *(layers.back()->m_sortedFeatures);
  vector<uint32_t> buffer;

  ParentGraph parentGraph;

  auto addEdge = [&](uint32_t childFeature, uint32_t parentFeature) {
    if (parentGraph.back().find(childFeature) == parentGraph.back().end())
      parentGraph.back()[childFeature] = parentFeature;
    buffer.push_back(childFeature);
  };

  for (size_t i = layers.size() - 1; i != 0; --i)
  {
    BailIfCancelled(m_cancellable);

    parentGraph.push_back({});
    FeaturesLayer parent(*layers[i]);
    if (i != layers.size() - 1)
      my::SortUnique(reachable);
    parent.m_sortedFeatures = &reachable;

    // The first condition is an optimization: it is enough to extract
    // the delayed features only once.
    parent.m_hasDelayedFeatures = (i == layers.size() - 1 && MayHaveDelayedFeatures(parent));

    FeaturesLayer child(*layers[i - 1]);
    child.m_hasDelayedFeatures = MayHaveDelayedFeatures(child);

    buffer.clear();
    matcher.Match(child, parent, addEdge);
    reachable.swap(buffer);
  }

  auto const & lowestLevel = reachable;

  IntersectionResult result;
  for (auto const & id : lowestLevel)
  {
    if (GetPath(id, layers, parentGraph, result))
      results.push_back(result);
  }
}

void FeaturesLayerPathFinder::FindReachableVerticesBottomUp(
    FeaturesLayerMatcher & matcher, vector<FeaturesLayer const *> const & layers,
    vector<IntersectionResult> & results)
{
  ASSERT(!layers.empty(), ());

  vector<uint32_t> reachable = *(layers.front()->m_sortedFeatures);
  vector<uint32_t> buffer;

  ParentGraph parentGraph;

  // It is possible that there are delayed features on the lowest level.
  // We do not know about them until the matcher has been called, so
  // they will be added in |addEdge|. On the other hand, if there is
  // only one level, we must make sure that it is nonempty.
  // This problem does not arise in the top-down pass because there
  // the last reached level is exactly the lowest one.
  vector<uint32_t> lowestLevel = reachable;
  // True iff |addEdge| works with the lowest level.
  bool first = true;

  auto addEdge = [&](uint32_t childFeature, uint32_t parentFeature) {
    if (parentGraph.front().find(childFeature) != parentGraph.front().end())
      return;
    parentGraph.front()[childFeature] = parentFeature;
    buffer.push_back(parentFeature);

    if (first)
      lowestLevel.push_back(childFeature);
  };

  for (size_t i = 0; i + 1 != layers.size(); ++i)
  {
    BailIfCancelled(m_cancellable);

    parentGraph.push_front({});
    FeaturesLayer child(*layers[i]);
    if (i != 0)
      my::SortUnique(reachable);
    child.m_sortedFeatures = &reachable;
    child.m_hasDelayedFeatures = (i == 0 && MayHaveDelayedFeatures(child));

    FeaturesLayer parent(*layers[i + 1]);
    parent.m_hasDelayedFeatures = MayHaveDelayedFeatures(parent);

    buffer.clear();
    matcher.Match(child, parent, addEdge);
    reachable.swap(buffer);

    first = false;
  }

  my::SortUnique(lowestLevel);

  IntersectionResult result;
  for (auto const & id : lowestLevel)
  {
    if (GetPath(id, layers, parentGraph, result))
      results.push_back(result);
  }
}
}  // namespace search
