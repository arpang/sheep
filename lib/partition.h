#pragma once

#include <algorithm>
#include <parallel/algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <queue>
#include <unordered_set>
#include <vector>

#include "graph_wrapper.h"
#include "jnode.h"
#include "jtree.h"
#include "stdafx.h"

uint32_t knuth_hash(uint32_t k) {
  uint32_t prime = 2654435761;
  return k * prime;
}

uint32_t cormen_hash(uint32_t k) {
  double A = 0.5 * (sqrt(5) - 1);
  uint32_t s = floor(A * pow(2,32));
  return k * s;
}

class Partition {
public:
  short num_parts;
  std::vector<short> parts;
  #define INVALID_PART (short)-1

  // OPTIONS
  bool const edge_balanced;
  double const balance_factor;

  // DERIVED PARAMETERS
  size_t max_component;

  inline Partition(std::vector<jnid_t> const &seq, JNodeTable &jnodes, short np,
      bool eb = true, double bf = 1.03) :
    num_parts(np), parts(jnodes.size(), INVALID_PART),
    edge_balanced(eb), balance_factor(bf), max_component()
  {
    size_t count = 0;
    if (!edge_balanced)
      count = jnodes.size();
    else
      for (jnid_t id = 0; id != jnodes.size(); ++id)
        count += jnodes.pst_weight(id);
    max_component = (count / num_parts) * balance_factor;

    // For each jnid_t, assign a part.
    //backwardPartition(jnodes);
    forwardPartition(jnodes);

    // Convert jnid_t-indexed parts to vid_t indexed parts.
    std::vector<short> tmp(*std::max_element(seq.cbegin(), seq.cend()) + 1, INVALID_PART);
    for (size_t i = 0; i != seq.size(); ++i)
      tmp.at(seq.at(i)) = parts.at(i);
    parts = std::move(tmp);
  }

  inline Partition(std::vector<jnid_t> const &seq, char const *filename) :
    num_parts(), parts(), edge_balanced(), balance_factor(), max_component()
  {
    readPartition(filename);
    num_parts = *std::max_element(parts.cbegin(), parts.cend());

    // Convert jnid_t-indexed parts to vid_t indexed parts.
    std::vector<short> tmp(*std::max_element(seq.cbegin(), seq.cend()) + 1, INVALID_PART);
    for (size_t i = 0; i != seq.size(); ++i)
      tmp.at(seq.at(i)) = parts.at(i);
    parts = std::move(tmp);
  }

  template <typename GraphType>
  inline Partition(GraphType const &graph, std::vector<vid_t> const &seq, short np, 
      bool eb = true, double bf = 1.03) :
    num_parts(np), parts(graph.getMaxVid() + 1, INVALID_PART),
    edge_balanced(eb), balance_factor(bf), max_component()
  {
    size_t count = edge_balanced ? 2 * graph.getEdges() : graph.getNodes();
    max_component = (count / num_parts) * balance_factor;
    fennel(graph, seq);
  }

  inline Partition(char const *filename, short np) :
    num_parts(np), parts(), edge_balanced(true), balance_factor(1.03), max_component()
  {
    fennel(filename);
  }

  // PARTITIONING ALGORITHMS
  inline void backwardPartition(JNodeTable const &jnodes) {
    std::vector<size_t> component_below(jnodes.size(), 0);
    for (jnid_t id = 0; id != jnodes.size(); ++id) {
      component_below.at(id) += edge_balanced ? jnodes.pst_weight(id) : 1;
      if (jnodes.parent(id) != INVALID_JNID)
        component_below.at(jnodes.parent(id)) += component_below.at(id);
    }

    jnid_t critical = std::distance(
      component_below.cbegin(),
      std::max_element(component_below.cbegin(), component_below.cend()));
    while (jnodes.kids(critical).size() != 0) {
      critical = *std::max_element(jnodes.kids(critical).cbegin(), jnodes.kids(critical).cend(),
        [&component_below](jnid_t const lhs, jnid_t const rhs) {
        return component_below.at(lhs) < component_below.at(rhs); });
      component_below.at(jnodes.parent(critical)) -= component_below.at(critical);
    }

    short cur_part = 0;
    size_t part_size = 0;
    while (critical != INVALID_JNID) {
      if (part_size + component_below.at(critical) < max_component) {
        parts.at(critical) = cur_part;
        part_size += component_below.at(critical);
      } else {
        parts.at(critical) = ++cur_part;
        part_size = component_below.at(critical);
      }
      critical = jnodes.parent(critical);
    }

    for (jnid_t id = jnodes.size() - 1; id != (jnid_t)-1; --id) {
      if (parts.at(id) == INVALID_PART)
        parts.at(id) = jnodes.parent(id) != INVALID_JNID ? parts.at(jnodes.parent(id)) : cur_part;
    }
  }

  inline void forwardPartition(JNodeTable &jnodes) {
    std::vector<size_t> part_size;

    // Classic algorithm modified for FFD binpacking.
    // 1. Count the uncut component below X.
    // 2. If component_below(X) > max_component, pack bins.
    // Obviously there is some subtlety the bin packing setup. Things you might do:
    // 1. Pack kids more aggressively instead of halting as soon as the component fits (see batchtree.cpp)
    // 2. Try packing half-size components since this is ideal for bin packing.
    // 3. Move to edge-weighted stuff if you want to minimize edge cuts. This is probably the best idea.
    // 4. Spend more time reasoning about optimization criteria for communication volume.
    std::vector<size_t> component_below(jnodes.size(), 0);
    for (jnid_t id = 0; id != jnodes.size(); ++id) {
      component_below.at(id) += edge_balanced ? jnodes.pst_weight(id) : 1;
      ////XXX Edge-balanced partitioning in one line? Unconfident; seemed bugged; check it.

      if (component_below.at(id) > max_component) {
        std::sort(jnodes.kids(id).begin(), jnodes.kids(id).end(), [&](jnid_t const lhs, jnid_t const rhs)
          { return component_below.at(lhs) > component_below.at(rhs); });
        //XXX A more sophisticated algorithm would backtrack and consider all cuts below id, not just kids.
        //It turns out in the trees we look at that there's very few options in direct children.

        do {
          // Try to pack kids.
          for (auto itr = jnodes.kids(id).cbegin(); component_below.at(id) > max_component &&
                    itr!= jnodes.kids(id).cend(); ++itr)
          {
            jnid_t const kid = *itr;
            assert(component_below.at(kid) <= max_component);
            if (parts.at(kid) != INVALID_PART) continue;

            // Find a part (bin) for this kid.
            for (short cur_part = 0; cur_part != (short) part_size.size(); ++cur_part) {
              // If kid packs...
              if (part_size.at(cur_part) + component_below.at(kid) <= max_component) {
                component_below.at(id) -= component_below.at(kid);
                part_size.at(cur_part) += component_below.at(kid);
                parts.at(kid) = cur_part;
                break;
              }
            }
          }
          // If kid packing fails, open a new part (bin)
          if (component_below.at(id) > max_component)
            part_size.push_back(0);
        } while(component_below.at(id) > max_component);
      }
      assert(component_below.at(id) <= max_component);
      if (jnodes.parent(id) != INVALID_JNID)
        component_below.at(jnodes.parent(id)) += component_below.at(id);
    }

    for (jnid_t id = jnodes.size() - 1; id != (jnid_t)-1; --id) {
      if (parts.at(id) == INVALID_PART && jnodes.parent(id) != INVALID_JNID)
        parts.at(id) = parts.at(jnodes.parent(id));

      while (parts.at(id) == INVALID_PART) {
        for (short cur_part = part_size.size() - 1; cur_part != -1; --cur_part) {
          if (part_size.at(cur_part) + component_below.at(id) <= max_component) {
            part_size.at(cur_part) += component_below.at(id);
            parts.at(id) = cur_part;
            break;
          }
        }
        if (parts.at(id) == INVALID_PART)
          part_size.push_back(0);
      }
    }
  }

  //XXX This has been somewhat compelling for reducing CV; consider why.
  inline void depthPartition(JNodeTable const &jnodes) {
    std::vector<size_t> depth(jnodes.size(), 0);
    for (jnid_t id = jnodes.size() - 1; id != (jnid_t)-1; --id)
      if (jnodes.parent(id) != INVALID_JNID)
        depth.at(id) = depth.at(jnodes.parent(id)) + 1;

    std::vector<jnid_t> jnids(jnodes.size());
    std::iota(jnids.begin(), jnids.end(), 0);
    __gnu_parallel::stable_sort(jnids.begin(), jnids.end(),
      [&](jnid_t const lhs, jnid_t const rhs) { return depth.at(lhs) > depth.at(rhs); });

    short cur_part = 0;
    size_t cur_size = 0;
    for (size_t idx = 0; idx != jnids.size(); ++idx) {
      parts.at(jnids.at(idx)) = cur_part;
      cur_size += edge_balanced ? jnodes.pst_weight(jnids.at(idx)) : 1;
      if (cur_size >= max_component) {
        ++cur_part;
        cur_size = 0;
      }
    }
  }

  //XXX This is practically anti-optimal...why?
  inline void heightPartition(JNodeTable const &jnodes) {
    std::vector<size_t> height(jnodes.size(), 0);
    for (jnid_t id = 0; id != jnodes.size(); ++id)
      if (jnodes.parent(id) != INVALID_JNID)
        height.at(jnodes.parent(id)) = std::max(height.at(jnodes.parent(id)), height.at(id) + 1);

    std::vector<jnid_t> jnids(jnodes.size());
    std::iota(jnids.begin(), jnids.end(), 0);
    __gnu_parallel::stable_sort(jnids.begin(), jnids.end(),
      [&](jnid_t const lhs, jnid_t const rhs) { return height.at(lhs) < height.at(rhs); });

    short cur_part = 0;
    size_t cur_size = 0;
    for (size_t idx = 0; idx != jnids.size(); ++idx) {
      parts.at(jnids.at(idx)) = cur_part;
      cur_size += edge_balanced ? jnodes.pst_weight(jnids.at(idx)) : 1;
      if (++cur_size == max_component) {
        ++cur_part;
        cur_size = 0;
      }
    }
  }

  inline void naivePartition(JNodeTable const &jnodes) {
    short cur_part = 0;
    size_t cur_size = 0;
    for (jnid_t id = 0; id != jnodes.size(); ++id) {
      parts.at(id) = cur_part;
      cur_size += edge_balanced ? jnodes.pst_weight(id) : 1;
      if (++cur_size == max_component) {
        ++cur_part;
        cur_size = 0;
      }
    }
  }

  inline void randomPartition(size_t vertex_count) {
    srand(time(nullptr));

    assert(parts.size() == 0);
    parts.resize(vertex_count);
    for (auto itr = parts.begin(); itr != parts.end(); ++itr)
      *itr = rand() % num_parts;
  }

  inline void readPartition(char const *filename) {
    std::ifstream stream(filename);
    short p;

    assert(parts.size() == 0);
    while (stream >> p)
      parts.push_back(p);
  }



  inline void print() const
  {
    short max_part = 0;
    size_t first_part = 0;
    size_t second_part = 0;

    for (short part : parts) {
      max_part = std::max(max_part, part);
      if (part == 0)
        first_part += 1;
      else if (part == 1)
        second_part += 1;
    }

    printf("Actually created %d partitions.\n", max_part + 1);
    printf("First two partition sizes: %zu and %zu\n", first_part, second_part);
  }

  template <typename GraphType>
  inline void evaluate(GraphType const &graph) const {
    srand(time(nullptr));

    size_t edges_cut = 0;
    size_t Vcom_vol = 0;
    size_t ECV_rand = 0;
    size_t ECV_hash = 0;

    for (auto nitr = graph.getNodeItr(); !nitr.isEnd(); ++nitr) {
      vid_t const X = *nitr;
      short const X_part = parts.at(X);
      assert(X_part != INVALID_PART);

      std::unordered_set<short> Vcom_vol_nbrs = {X_part};
      std::unordered_set<short> ECV_rand_nbrs = {};
      std::unordered_set<short> ECV_hash_nbrs = {};

      for (auto eitr = graph.getEdgeItr(X); !eitr.isEnd(); ++eitr) {
        vid_t const Y = *eitr;
        short const Y_part = parts.at(Y);
        assert(Y_part != INVALID_PART);

        if (X < Y && X_part != Y_part) ++edges_cut;
        Vcom_vol_nbrs.insert(Y_part);
        ECV_rand_nbrs.insert((rand() % 2) ? X_part : Y_part);
        ECV_hash_nbrs.insert(knuth_hash(X) < knuth_hash(Y) ? X_part : Y_part);

      }
      Vcom_vol += Vcom_vol_nbrs.size() - 1;
      ECV_rand += ECV_rand_nbrs.size() - 1;
      ECV_hash += ECV_hash_nbrs.size() - 1;
    }

    //XXX Remember graph.getEdges includes self-edges for some graphs.
    printf("edges cut: %zu (%f%%)\n", edges_cut, (double) edges_cut / graph.getEdges());
    printf("Vcom. vol: %zu (%f%%)\n", Vcom_vol, (double) Vcom_vol / graph.getEdges());
    printf("ECV(rand): %zu (%f%%)\n", ECV_rand, (double) ECV_rand / graph.getEdges());
    printf("ECV(hash): %zu (%f%%)\n", ECV_hash, (double) ECV_hash / graph.getEdges());
  }

  template <typename GraphType>
  inline void evaluate(GraphType const &graph, std::vector<vid_t> const &seq) const {
    evaluate(graph);

    std::vector<jnid_t> pos(*std::max_element(seq.cbegin(), seq.cend()) + 1, INVALID_JNID);
    for (size_t i = 0; i != seq.size(); ++i)
      pos[seq[i]] = i;

    size_t ECV_down = 0;
    size_t ECV_up = 0;
    //std::vector<size_t> part_count(*std::max_element(parts.cbegin(), parts.cend()) + 1);

    for (auto nitr = graph.getNodeItr(); !nitr.isEnd(); ++nitr) {
      vid_t const X = *nitr;
      jnid_t const X_pos = pos.at(X);
      short const X_part = parts.at(X);
      assert(X_part != INVALID_PART);

      std::unordered_set<short> ECV_down_nbrs = {};
      std::unordered_set<short> ECV_up_nbrs = {};

      for (auto eitr = graph.getEdgeItr(X); !eitr.isEnd(); ++eitr) {
        vid_t const Y = *eitr;
        jnid_t const Y_pos = pos.at(Y);
        short const Y_part = parts.at(Y);
        assert(Y_part != INVALID_PART);

        ECV_down_nbrs.insert((X_pos < Y_pos) ? X_part : Y_part);
        ECV_up_nbrs.insert((X_pos > Y_pos) ? X_part : Y_part);
        //part_count.at(X_pos < Y_pos ? X_part : Y_part)++;
      }
      ECV_down += ECV_down_nbrs.size() - 1;
      ECV_up += ECV_up_nbrs.size() - 1;
    }

    printf("ECV(down): %zu (%f%%)\n", ECV_down, (double) ECV_down / graph.getEdges());
    printf("ECV(up)  : %zu (%f%%)\n", ECV_up, (double) ECV_up / graph.getEdges());
    //for (size_t count : part_count)
      //printf("part: %zu\n", count);
  }

  template <typename GraphType>
  inline void writePartitionedGraph(GraphType const &graph, std::vector<vid_t> const &seq, char const *const output_prefix) const {
    short const max_part = *std::max_element(parts.cbegin(), parts.cend());
    assert(max_part < 100);
    std::vector<std::ofstream*> output_streams;
    for (short p = 0; p != max_part + 1; ++p) {
      char *output_filename = (char*)malloc(strlen(output_prefix) + 3);
      sprintf(output_filename, "%s%02d", output_prefix, p);
      output_streams.emplace_back(new std::ofstream(output_filename, std::ios::trunc));
      free(output_filename);
    }
    
    std::vector<jnid_t> pos(*std::max_element(seq.cbegin(), seq.cend()) + 1, INVALID_JNID);
    for (size_t i = 0; i != seq.size(); ++i)
      pos[seq[i]] = i;

    for (auto nitr = graph.getNodeItr(); !nitr.isEnd(); ++nitr) {
      vid_t const X = *nitr;
      jnid_t const X_pos = pos.at(X);
      short const X_part = parts.at(X);
      assert(X_part != INVALID_PART);

      for (auto eitr = graph.getEdgeItr(X); !eitr.isEnd(); ++eitr) {
        vid_t const Y = *eitr;
        if (X >= Y) continue;

        jnid_t const Y_pos = pos.at(Y);
        short const Y_part = parts.at(Y);
        assert(Y_part != INVALID_PART);

        short edge_part = X_pos < Y_pos ? X_part : Y_part;

        *output_streams.at(edge_part) << X << " " << Y << std::endl;
      }
    }

    for (std::ofstream *stream : output_streams)
      delete stream;
  }



  template <typename GraphType>
  void fennel(GraphType const &graph, std::vector<vid_t> const &seq) {
    double const n = graph.getNodes();
    double const m = 2 * graph.getEdges(); // # of DIRECTED edges; getEdges() returns UNDIRECTED#.
    double const k = num_parts;

    double const y = 1.5;
    double const a = edge_balanced ?
      n * pow(k / m, y) : // From KDD14 paper.
      m * (pow(k, y - 1.0) / pow(n, y)); // From original FENNEL paper.

    std::vector<double> part_value;
    std::vector<double> part_size(num_parts, 0.0);

    //for (vid_t const X : seq) {
    for (auto nitr = graph.getNodeItr(); !nitr.isEnd(); ++nitr) {
      vid_t const X = *nitr;
      double X_weight = edge_balanced ? ((double) graph.getDeg(X)) : 1.0;

      part_value.assign(num_parts, 0.0);
      for (auto X_itr = graph.getEdgeItr(X); !X_itr.isEnd(); ++X_itr) {
        vid_t const Y = *X_itr;
        //if (X <= Y) continue;
        if (parts[Y] != INVALID_PART)
          part_value[parts[Y]] += 1.0;
      }

      short max_part = 0;
      double max_value = std::numeric_limits<double>::lowest();
      for (short p = 0; p != num_parts; ++p) {
        if (part_size[p] + X_weight > max_component) continue; // Hard balance limit.

        //double p_cost = a * y * pow(part_size[p], y - 1.0); // From original FENNEL paper.
        double p_cost = a * pow(part_size[p] + X_weight, y) - a * pow(part_size[p], y);
        double p_value = part_value[p] - p_cost;
          if (p_value > max_value) {
          max_part = p;
          max_value = p_value;
        }

        if (part_size[p] == 0.0) break; // Everything will be 0.0 after this point.
      }
      parts[X] = max_part;
      part_size[max_part] += X_weight;
    }
  }

  void fennel(char const *const filename) {
    assert(edge_balanced == true);

    vid_t max_vid = 4036529;
    size_t edge_count = 34681189;

    struct xs1 {
	    unsigned tail;
	    unsigned head;
	    float weight;
    };
    xs1 buf;
    /*
    {
    std::ifstream stream(filename, std::ios::binary);
    while (!stream.eof()) {
      stream.read((char*)&buf, sizeof(xs1));
      if (buf.tail > max_vid)
        max_vid = buf.tail;
      if (buf.head > max_vid)
        max_vid = buf.head;
      ++edge_count;
    }
    }
    */

    parts.assign(edge_count + 1, INVALID_PART);
    max_component = (edge_count/num_parts)*balance_factor;

    double const n = 3997962;
    double const m = 2 * edge_count; // # of DIRECTED edges; getEdges() returns UNDIRECTED#.
    double const k = num_parts;

    double const y = 1.5;
    double const a = m * (pow(k, y - 1.0) / pow(n, y));

    std::vector<double> part_value;
    std::vector<double> part_size(num_parts, 0.0);
    std::vector<bool> touches_part(num_parts * (max_vid + 1));

    std::ifstream stream(filename, std::ios::binary);
    for (size_t eid = 0; !stream.eof(); ++eid) {
      stream.read((char*)&buf, sizeof(xs1));
      vid_t const X = buf.tail;
      vid_t const Y = buf.head;

      part_value.assign(num_parts, 0.0);
      for (short p = 0; k != num_parts; ++p) {
        if (touches_part.at(num_parts * X + p) == true)
          part_value[p] += 1.0;
        if (touches_part.at(num_parts * Y + p) == true)
          part_value[p] += 1.0;
      }

      short max_part = 0;
      double max_value = std::numeric_limits<double>::lowest();
      for (short p = 0; p != num_parts; ++p) {
        if (part_size[p] + 1.0 > max_component) continue; // Hard balance limit.

        double p_cost = a * pow(part_size[p] + 1.0, y) - a * pow(part_size[p], y);
        double p_value = part_value[p] - p_cost;
          if (p_value > max_value) {
          max_part = p;
          max_value = p_value;
        }

        if (part_size[p] == 0.0) break; // Everything will be 0.0 after this point.
      }

      parts[eid] = max_part;
      part_size[max_part] += 1.0;
      touches_part.at(num_parts * X + max_part) = true;
      touches_part.at(num_parts * X + max_part) = true;
    }
  }
};
