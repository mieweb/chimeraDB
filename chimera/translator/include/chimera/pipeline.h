#pragma once

#include <vector>

#include "chimera/bson.h"

namespace chimera {

// A pipeline is split in two before it runs, because only its leading `$match`
// stages can be answered by SQL. Once a stage has reshaped the documents there
// is no `doc` column left to compile a filter against, so everything from that
// point on is evaluated over documents this process already holds.
struct Pipeline {
  Bson prefilter;            // the leading $match stages, merged; feed to the load
  std::vector<Bson> stages;  // whatever follows, in order
};

// Raises on any stage outside the supported set rather than dropping it — a
// silently ignored stage returns the wrong answer, which is worse than an error.
Pipeline split_pipeline(const bson_t* pipeline);

// Runs the post-filter stages: $project, $sort, $limit, $skip, $count, $group.
std::vector<Bson> run_stages(std::vector<Bson> documents, const std::vector<Bson>& stages);

}  // namespace chimera
