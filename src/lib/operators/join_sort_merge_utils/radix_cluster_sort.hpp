#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "resolve_type.hpp"
#include "column_materializer.hpp"

namespace opossum {

/*
*
* Performs radix clustering for the sort merge join. The radix clustering algorithm clusters on the basis
* of the least significant bits of the values because the values there are much more evenly distributed than for the
* most significant bits. As a result, equal values always get moved to the same cluster and the clusters are
* sorted in themselves but not in between the clusters. This is okay for the equi join, because we are only interested
* in equality. In the case of a non-equi join however, complete sortedness is required, because join matches exist
* beyond cluster borders. Therefore, the clustering defaults to a range clustering algorithm for the non-equi-join.
* General clustering process:
* -> Input chunks are materialized and sorted. Every value is stored together with its row id.
* -> Then, either radix clustering or range clustering is performed.
* -> At last, the resulting clusters are sorted.
*
* Radix clustering example:
* cluster_count = 4
* bits for 4 clusters: 2
*
*   000001|01
*   000000|11
*          ˆ right bits are used for clustering
*
**/
template <typename T>
class RadixClusterSort {
 public:
  RadixClusterSort(const std::shared_ptr<const Table> left, const std::shared_ptr<const Table> right,
                     std::pair<std::string, std::string> column_names, bool equi_case, size_t cluster_count)
    : _input_table_left{left}, _input_table_right{right}, _left_column_name{column_names.first},
      _right_column_name{column_names.second}, _equi_case{equi_case},
      _cluster_count{cluster_count} {
    DebugAssert(cluster_count > 0, "cluster_count must be > 0");
    DebugAssert((cluster_count & (cluster_count - 1)) == 0,
                "cluster_count must be a power of two, i.e. 1, 2, 4, 8...");
    DebugAssert(left != nullptr, "left input operator is null");
    DebugAssert(right != nullptr, "right input operator is null");
  }

  virtual ~RadixClusterSort() = default;

 protected:
  // Input parameters
  std::shared_ptr<const Table> _input_table_left;
  std::shared_ptr<const Table> _input_table_right;
  const std::string _left_column_name;
  const std::string _right_column_name;
  bool _equi_case;

  // The cluster count must be a power of two, i.e. 1, 2, 4, 8, 16, ...
  // It is asserted to be a power of two in the constructor.
  size_t _cluster_count;

  std::shared_ptr<MaterializedColumnList<T>> _output_left;
  std::shared_ptr<MaterializedColumnList<T>> _output_right;

  // Radix calculation for arithmetic types
  template <typename T2>
  static typename std::enable_if<std::is_arithmetic<T2>::value, uint32_t>::type get_radix(T2 value,
                                                                                          uint32_t radix_bitmask) {
    return static_cast<uint32_t>(value) & radix_bitmask;
  }

  // Radix calculation for non-arithmetic types
  template <typename T2>
  static typename std::enable_if<std::is_same<T2, std::string>::value, uint32_t>::type get_radix(T2 value,
                                                                                           uint32_t radix_bitmask) {
    auto result = reinterpret_cast<const uint32_t*>(value.c_str());
    return *result & radix_bitmask;
  }

  /**
  * Determines the total size of a materialized column list.
  **/
  static size_t _materialized_table_size(std::shared_ptr<MaterializedColumnList<T>> table) {
    size_t total_size = 0;
    for (auto chunk : *table) {
      total_size += chunk->size();
    }

    return total_size;
  }

  /**
  * Concatenates multiple materialized chunks to a single materialized column chunk.
  **/
  static std::shared_ptr<MaterializedColumnList<T>> _concatenate_chunks(
                                                             std::shared_ptr<MaterializedColumnList<T>> input_chunks) {
    auto output_table = std::make_shared<MaterializedColumnList<T>>(1);
    (*output_table)[0] = std::make_shared<MaterializedColumn<T>>();

    // Reserve the required space and move the data to the output
    auto output_chunk = (*output_table)[0];
    output_chunk->reserve(_materialized_table_size(input_chunks));
    for (auto& chunk : *input_chunks) {
      output_chunk->insert(output_chunk->end(), chunk->begin(), chunk->end());
    }

    return output_table;
  }

  /**
  * Performs the clustering on a materialized table using a clustering function that determines for each
  * value the appropriate cluster id. This is how the clustering works:
  * -> Count for each chunk how many of its values belong in each of the clusters using histograms.
  * -> Aggregate the per-chunk histograms to a histogram for the whole table. For each chunk it is noted where
  *    it will be inserting values in each cluster.
  * -> Reserve the appropriate space for each output cluster to avoid ongoing vector resizing.
  * -> At last, each value of each chunk is moved to the appropriate cluster.
  **/
  std::shared_ptr<MaterializedColumnList<T>> _cluster(std::shared_ptr<MaterializedColumnList<T>> input_chunks,
                                                                    std::function<size_t(const T&)> clusterer) {
    auto output_table = std::make_shared<MaterializedColumnList<T>>(_cluster_count);

    // a mutex for each cluster for parallel clustering
    std::vector<std::mutex> cluster_mutexes(_cluster_count);

    // Reserve the appropriate output space for the clusters by assuming a uniform distribution
    for (size_t cluster_id = 0; cluster_id < _cluster_count; ++cluster_id) {
      auto cluster_size = _materialized_table_size(input_chunks) / _cluster_count;
      (*output_table)[cluster_id] = std::make_shared<MaterializedColumn<T>>();
      (*output_table)[cluster_id]->reserve(cluster_size);
    }

    // Move each entry into its appropriate cluster in parallel
    std::vector<std::shared_ptr<AbstractTask>> cluster_jobs;
    for (size_t chunk_number = 0; chunk_number < input_chunks->size(); ++chunk_number) {
      auto job = std::make_shared<JobTask>([chunk_number, &cluster_mutexes, &output_table, &input_chunks, &clusterer] {
        for (auto& entry : *(*input_chunks)[chunk_number]) {
          auto cluster_id = clusterer(entry.value);
          cluster_mutexes[cluster_id].lock();
          (*output_table)[cluster_id]->push_back(entry);
          cluster_mutexes[cluster_id].unlock();
        }
      });
      cluster_jobs.push_back(job);
      job->schedule();
    }

    CurrentScheduler::wait_for_tasks(cluster_jobs);

    return output_table;
  }

  /**
  * Performs least significant bit radix clusterering which is used in the equi join case.
  * Note: if we used the most significant bits, we could also use this for non-equi joins.
  * Then, however we would have to deal with skewed clusters. Other ideas:
  * - hand select the clustering bits based on statistics.
  * - consolidate clusters in order to reduce skew.
  **/
  std::shared_ptr<MaterializedColumnList<T>> _radix_cluster(std::shared_ptr<MaterializedColumnList<T>> input_chunks) {
    auto radix_bitmask = _cluster_count - 1;
    return _cluster(input_chunks, [=] (const T& value) {
      return get_radix<T>(value, radix_bitmask);
    });
  }

  /**
  * Picks sample values from a materialized table that are used to determine cluster range bounds.
  **/
  void _pick_sample_values(std::vector<std::map<T, size_t>>& sample_values,
                           std::shared_ptr<MaterializedColumnList<T>> table) {
    // Note:
    // - The materialized chunks are sorted.
    // - Between the chunks there is no order
    // - Every chunk can contain values for every cluster
    // - To sample for range border values we look at the position where the values for each cluster
    //   would start if every chunk had an even values distribution for every cluster.
    // - Later, these values are aggregated to determine the actual cluster borders
    for (size_t chunk_number = 0; chunk_number < table->size(); ++chunk_number) {
      auto chunk_values = (*table)[chunk_number];
      for (size_t cluster_id = 0; cluster_id < _cluster_count - 1; ++cluster_id) {
        auto pos = chunk_values->size() * (cluster_id + 1) / static_cast<float>(_cluster_count);
        auto index = static_cast<size_t>(pos);
        ++sample_values[cluster_id][(*chunk_values)[index].value];
      }
    }
  }

  /**
  * Performs the radix cluster sort for the non-equi case (>, >=, <, <=) which requires the complete table to
  * be sorted and not only the clusters in themselves.
  **/
  std::pair<std::shared_ptr<MaterializedColumnList<T>>, std::shared_ptr<MaterializedColumnList<T>>> _range_cluster(
                                                              std::shared_ptr<MaterializedColumnList<T>> input_left,
                                                              std::shared_ptr<MaterializedColumnList<T>> input_right) {
    std::vector<std::map<T, size_t>> sample_values(_cluster_count - 1);

    _pick_sample_values(sample_values, input_left);
    _pick_sample_values(sample_values, input_right);

    // Pick the most common sample values for each cluster for the split values.
    // The last cluster does not need a split value because it covers all values that are bigger than all split values
    // Note: the split values mark the ranges of the clusters.
    // A split value is the end of a range and the start of the next one.
    std::vector<T> split_values(_cluster_count - 1);
    for (size_t cluster_id = 0; cluster_id < _cluster_count - 1; ++cluster_id) {
      // Pick the values with the highest count
      split_values[cluster_id] = std::max_element(sample_values[cluster_id].begin(),
                                                  sample_values[cluster_id].end(),
        // second is the count of the value
        [] (auto& a, auto& b) {
          return a.second < b.second;
      })->second;
    }

    // Implements range clustering
    auto cluster_count = _cluster_count;
    auto clusterer = [cluster_count, &split_values](const T& value) {
      // Find the first split value that is greater or equal to the entry.
      // The split values are sorted in ascending order.
      // Note: can we do this faster? (binary search?)
      for (size_t cluster_id = 0; cluster_id < cluster_count - 1; ++cluster_id) {
        if (value <= split_values[cluster_id]) {
          return cluster_id;
        }
      }

      // The value is greater than all split values, which means it belongs in the last cluster.
      return cluster_count - 1;
    };

    auto output_left = _cluster(input_left, clusterer);
    auto output_right = _cluster(input_right, clusterer);

    return std::pair<std::shared_ptr<MaterializedColumnList<T>>,
                     std::shared_ptr<MaterializedColumnList<T>>>(output_left, output_right);
  }

  /**
  * Sorts all clusters of a materialized table.
  **/
  void _sort_clusters(std::shared_ptr<MaterializedColumnList<T>> clusters) {
    for (auto cluster : *clusters) {
      std::sort(cluster->begin(), cluster->end(), [](auto& left, auto& right) {
        return left.value < right.value;
      });
    }
  }

 public:
  /**
  * Executes the clustering and sorting.
  **/
  std::pair<std::shared_ptr<MaterializedColumnList<T>>, std::shared_ptr<MaterializedColumnList<T>>> execute() {
    // Sort the chunks of the input tables in the non-equi cases
    ColumnMaterializer<T> column_materializer(!_equi_case);
    auto chunks_left = column_materializer.materialize(_input_table_left, _left_column_name);
    auto chunks_right = column_materializer.materialize(_input_table_right, _right_column_name);

    if (_cluster_count == 1) {
      _output_left = _concatenate_chunks(chunks_left);
      _output_right = _concatenate_chunks(chunks_right);
    } else if (_equi_case) {
      _output_left = _radix_cluster(chunks_left);
      _output_right = _radix_cluster(chunks_right);
    } else {
      auto result = _range_cluster(chunks_left, chunks_right);
      _output_left = result.first;
      _output_right = result.second;
    }

    // Sort each cluster (right now std::sort -> but maybe can be replaced with
    // an algorithm more efficient, if subparts are already sorted [InsertionSort?!])
    _sort_clusters(_output_left);
    _sort_clusters(_output_right);

    DebugAssert(_materialized_table_size(_output_left) == _input_table_left->row_count(),
                "left output has wrong size");
    DebugAssert(_materialized_table_size(_output_right) == _input_table_right->row_count(),
                "right output has wrong size");

    return std::make_pair(_output_left, _output_right);
  }
};

}  // namespace opossum
