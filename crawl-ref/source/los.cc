/*
 *  File:        los.cc
 *  Summary:     Line-of-sight algorithm.
 *
 *
 *
 * == Definition of visibility ==
 *
 * Two cells are in view of each other if there is any straight
 * line that meets both cells and that doesn't meet any opaque
 * cell in between, and if the cells are in LOS range of each
 * other.
 *
 * Here, to "meet" a cell means to intersect the interiour. In
 * particular, rays can pass between to diagonally adjacent
 * walls (as can the player).
 *
 * == Terminology ==
 *
 * A _ray_ is a line, specified by starting point (accx, accy)
 * and slope. A ray determines its _footprint_: the sequence of
 * cells whose interiour it meets.
 *
 * Any prefix of the footprint of a ray is called a _cellray_.
 *
 * For the purposes of LOS calculation, only the footprints
 * are relevant, but rays are also used for shooting beams,
 * which may travel beyond LOS and which can be reflected.
 * See ray.cc.
 *
 * == Overview ==
 *
 * At first use, the LOS code makes some precomputations,
 * filling a list of all relevant rays in one quadrant,
 * and filling data structures that allow calculating LOS
 * in a quadrant without checking each ray.
 *
 * The code provides functions for filling LOS information
 * around a given center efficiently, and for querying rays
 * between two given cells.
 */

#include "AppHdr.h"
REVISION("$Rev$");

#include "los.h"

#include <cmath>
#include <algorithm>

#include "bitary.h"
#include "debug.h"
#include "directn.h"
#include "externs.h"
#include "losparam.h"
#include "ray.h"
#include "state.h"
#include "stuff.h"
#include "terrain.h"

#define LONGSIZE (sizeof(unsigned long)*8)
#define LOS_MAX_RANGE 9

// These determine what rays are cast in the precomputation,
// and affect start-up time significantly.
// XXX: Argue that these values are sufficient.
#define LOS_MAX_ANGLE (2*LOS_MAX_RANGE-2)
#define LOS_INTERCEPT_MULT (2)

// the following two constants represent the 'middle' of the sh array.
// since the current shown area is 19x19, centering the view at (9,9)
// means it will be exactly centered.
// This is done to accomodate possible future changes in viewable screen
// area - simply change sh_xo and sh_yo to the new view center.
const int sh_xo = 9;            // X and Y origins for the sh array
const int sh_yo = 9;
const coord_def sh_o = coord_def(sh_xo, sh_yo);

// These store all unique (in terms of footprint) full rays.
// The footprint of ray=fullray[i] consists of ray.length cells,
// stored in ray_coords[ray.start..ray.length-1].
// These are filled during precomputation (_register_ray).
// XXX: fullrays is not needed anymore after precomputation.
struct los_ray;
std::vector<los_ray> fullrays;
std::vector<coord_def> ray_coords;

// These store all unique minimal cellrays. For each i,
// cellray i ends in cellray_ends[i] and passes through
// thoses cells p that have blockrays(p)[i] set. In other
// words, blockrays(p)[i] is set iff an opaque cell p blocks
// the cellray with index i.
std::vector<coord_def> cellray_ends;
typedef FixedArray<bit_array*, LOS_MAX_RANGE+1, LOS_MAX_RANGE+1> blockrays_t;
blockrays_t blockrays;

// We also store the minimal cellrays by target position
// for efficient retrieval by find_ray.
// XXX: Consider condensing this representation.
struct cellray;
FixedArray<std::vector<cellray>, LOS_MAX_RANGE+1, LOS_MAX_RANGE+1> min_cellrays;

// Temporary arrays used in losight() to track which rays
// are blocked or have seen a smoke cloud.
// Allocated when doing the precomputations.
bit_array *dead_rays     = NULL;
bit_array *smoke_rays    = NULL;

class quadrant_iterator : public rectangle_iterator
{
public:
    quadrant_iterator()
        : rectangle_iterator(coord_def(0,0),
                             coord_def(LOS_MAX_RANGE, LOS_MAX_RANGE))
    {
    }
};

void clear_rays_on_exit()
{
   delete dead_rays;
   delete smoke_rays;
   for (quadrant_iterator qi; qi; qi++)
       delete blockrays(*qi);
}

// Pre-squared LOS radius.
#define LOS_RADIUS2 (LOS_RADIUS * LOS_RADIUS + 1)

int _los_radius_squared = LOS_RADIUS2;

void setLOSRadius(int newLR)
{
    _los_radius_squared = newLR * newLR + 1*1;
}

// XXX: just for monster_los
int get_los_radius_squared()
{
    return _los_radius_squared;
}

bool double_is_zero(const double x)
{
    return (x > -EPSILON_VALUE) && (x < EPSILON_VALUE);
}

struct los_ray : public ray_def
{
    // The footprint of this ray is stored in
    // ray_coords[start..start+length-1].
    unsigned int start;
    unsigned int length;

    los_ray(double ax, double ay, double s)
        : ray_def(ax, ay, s), length(0)
    {
    }

    // Shoot a ray from the given start point (accx, accy) with the given
    // slope, bounded by the given pre-squared LOS radius.
    // Returns the cells it travels through, excluding the origin.
    std::vector<coord_def> footprint(int radius2)
    {
        std::vector<coord_def> cs;
        los_ray copy = *this;
        coord_def c;
        int cellnum;
        for (cellnum = 0; true; ++cellnum)
        {
            copy.raw_advance_pos();
            c = copy.pos();
            if (c.abs() > radius2)
                break;
            cs.push_back(c);
        }
        return cs;
    }

    coord_def operator[](unsigned int i)
    {
        ASSERT(0 <= i && i < length);
        return ray_coords[start+i];
    }
};

// Check if the passed rays have identical footprint.
static bool _is_same_ray(los_ray ray, std::vector<coord_def> newray)
{
    if (ray.length != newray.size())
        return false;
    for (unsigned int i = 0; i < ray.length; i++)
        if (ray[i] != newray[i])
            return false;
    return true;
}

// Check if the passed ray has already been created.
static bool _is_duplicate_ray(std::vector<coord_def> newray)
{
    for (unsigned int i = 0; i < fullrays.size(); ++i)
        if (_is_same_ray(fullrays[i], newray))
            return true;
    return false;
}

// A cellray given by fullray and index of end-point.
struct cellray
{
    // A cellray passes through cells ray_coords[ray.start..ray.start+end].
    los_ray ray;
    unsigned int end; // Relative index (inside ray) of end cell.

    cellray(const los_ray& r, unsigned int e)
        : ray(r), end(e), imbalance(-1), slope_diff(-1)
    {
    }

    // The end-point's index inside ray_coord.
    int index() const { return (ray.start + end); }

    // The end-point.
    coord_def target() const { return ray_coords[index()]; }

    // XXX: Currently ray/cellray[0] is the first point outside the origin.
    coord_def operator[](unsigned int i)
    {
        ASSERT(0 <= i && i <= end);
        return ray_coords[ray.start+i];
    }

    // Parameters used in find_ray. These need to be calculated
    // only for the minimal cellrays.
    int imbalance;
    double slope_diff;

    void calc_params();
};

// Compare two cellrays to the same target.
// This determines which ray is considered better by find_ray,
// used with list::sort.
// Returns true if a is strictly better than b, false else.
bool _is_better(const cellray& a, const cellray& b)
{
    // Only compare cellrays with equal target.
    ASSERT(a.target() == b.target());
    // calc_params() has been called.
    ASSERT(a.imbalance >= 0 && b.imbalance >= 0);
    if (a.imbalance < b.imbalance)
        return (true);
    else if (a.imbalance > b.imbalance)
        return (false);
    else
        return (a.slope_diff < b.slope_diff);
}

enum compare_type
{
    C_SUBRAY,
    C_SUPERRAY,
    C_NEITHER
};

// Check whether one of the passed cellrays is a subray of the
// other in terms of footprint.
compare_type _compare_cellrays(const cellray& a, const cellray& b)
{
    if (a.target() != b.target())
        return C_NEITHER;

    int cura = a.ray.start;
    int curb = b.ray.start;
    int enda = cura + a.end;
    int endb = curb + b.end;
    bool maybe_sub = true;
    bool maybe_super = true;

    while (cura < enda && curb < endb && (maybe_sub || maybe_super))
    {
        coord_def pa = ray_coords[cura];
        coord_def pb = ray_coords[curb];
        if (pa.x > pb.x || pa.y > pb.y)
        {
            maybe_super = false;
            curb++;
        }
        if (pa.x < pb.x || pa.y < pb.y)
        {
            maybe_sub = false;
            cura++;
        }
        if (pa == pb)
        {
            cura++;
            curb++;
        }
    }
    maybe_sub = maybe_sub && (cura == enda);
    maybe_super = maybe_super && (curb == endb);

    if (maybe_sub)
        return C_SUBRAY;    // includes equality
    else if (maybe_super)
        return C_SUPERRAY;
    else
        return C_NEITHER;
}

// Determine all minimal cellrays.
// They're stored globally by target in min_cellrays,
// and returned as a list of indices into ray_coords.
static std::vector<int> _find_minimal_cellrays()
{
    FixedArray<std::list<cellray>, LOS_MAX_RANGE+1, LOS_MAX_RANGE+1> minima;
    std::list<cellray>::iterator min_it;

    for (unsigned int r = 0; r < fullrays.size(); ++r)
    {
        los_ray ray = fullrays[r];
        for (unsigned int i = 0; i < ray.length; ++i)
        {
            // Is the cellray ray[0..i] duplicated so far?
            bool dup = false;
            cellray c(ray, i);
            std::list<cellray>& min = minima(c.target());

            bool erased = false;
            for (min_it = min.begin();
                 min_it != min.end() && !dup; )
            {
                switch(_compare_cellrays(*min_it, c))
                {
                case C_SUBRAY:
                    dup = true;
                    break;
                case C_SUPERRAY:
                    min_it = min.erase(min_it);
                    erased = true;
                    // clear this should be added, but might have
                    // to erase more
                    break;
                case C_NEITHER:
                default:
                    break;
                }
                if (!erased)
                    min_it++;
                else
                    erased = false;
            }
            if (!dup)
                min.push_back(c);
        }
    }

    std::vector<int> result;
    for (quadrant_iterator qi; qi; qi++)
    {
        std::list<cellray>& min = minima(*qi);
        for (min_it = min.begin(); min_it != min.end(); min_it++)
        {
            // Calculate imbalance and slope difference for sorting.
            min_it->calc_params();
            result.push_back(min_it->index());
        }
        min.sort(_is_better);
        min_cellrays(*qi) = std::vector<cellray>(min.begin(), min.end());
    }
    return result;
}

// Create and register the ray defined by the arguments.
static void _register_ray(double accx, double accy, double slope)
{
    los_ray ray = los_ray(accx, accy, slope);
    std::vector<coord_def> coords = ray.footprint(LOS_RADIUS2);

    if (_is_duplicate_ray(coords))
        return;

    ray.start = ray_coords.size();
    ray.length = coords.size();
    for (unsigned int i = 0; i < coords.size(); i++)
        ray_coords.push_back(coords[i]);
    fullrays.push_back(ray);
}

static void _create_blockrays()
{
    // First, we calculate blocking information for all cell rays.
    // Cellrays are numbered according to the index of their end
    // cell in ray_coords.
    const int n_cellrays = ray_coords.size();
    blockrays_t all_blockrays;
    for (quadrant_iterator qi; qi; qi++)
        all_blockrays(*qi) = new bit_array(n_cellrays);

    for (unsigned int r = 0; r < fullrays.size(); ++r)
    {
        los_ray ray = fullrays[r];
        for (unsigned int i = 0; i < ray.length; ++i)
        {
            // Every cell is contained in (thus blocks)
            // all following cellrays.
            for (unsigned int j = i + 1; j < ray.length; ++j)
                all_blockrays(ray[i])->set(ray.start + j);
        }
    }

    // We've built the basic blockray array; now compress it, keeping
    // only the nonduplicated cellrays.

    // Determine minimal cellrays and store their indices in ray_coords.
    std::vector<int> min_indices = _find_minimal_cellrays();
    const int n_min_rays         = min_indices.size();
    cellray_ends.resize(n_min_rays);
    for (int i = 0; i < n_min_rays; ++i)
        cellray_ends[i] = ray_coords[min_indices[i]];

    // Compress blockrays accordingly.
    for (quadrant_iterator qi; qi; qi++)
    {
        blockrays(*qi) = new bit_array(n_min_rays);
        for (int i = 0; i < n_min_rays; ++i)
            blockrays(*qi)->set(i, all_blockrays(*qi)
                                   ->get(min_indices[i]));
    }

    // We can throw away all_blockrays now.
    for (quadrant_iterator qi; qi; qi++)
        delete all_blockrays(*qi);

    dead_rays  = new bit_array(n_min_rays);
    smoke_rays = new bit_array(n_min_rays);

#ifdef DEBUG_DIAGNOSTICS
    mprf( MSGCH_DIAGNOSTICS, "Cellrays: %d Fullrays: %u Minimal cellrays: %u",
          n_cellrays, fullrays.size(), n_min_rays );
#endif
}

static int _gcd( int x, int y )
{
    int tmp;
    while (y != 0)
    {
        x %= y;
        tmp = x;
        x = y;
        y = tmp;
    }
    return x;
}

bool complexity_lt( const std::pair<int,int>& lhs,
                    const std::pair<int,int>& rhs )
{
    return lhs.first * lhs.second < rhs.first * rhs.second;
}

// Cast all rays
void raycast()
{
    static bool done_raycast = false;
    if (done_raycast)
        return;

    // Creating all rays for first quadrant
    // We have a considerable amount of overkill.
    done_raycast = true;

    // register perpendiculars FIRST, to make them top choice
    // when selecting beams
    _register_ray(0.5, 0.5, 1000.0);
    _register_ray(0.5, 0.5, 0.0);

    // For a slope of M = y/x, every x we move on the X axis means
    // that we move y on the y axis. We want to look at the resolution
    // of x/y: in that case, every step on the X axis means an increase
    // of 1 in the Y axis at the intercept point. We can assume gcd(x,y)=1,
    // so we look at steps of 1/y.

    // Changing the order a bit. We want to order by the complexity
    // of the beam, which is log(x) + log(y) ~ xy.
    std::vector<std::pair<int,int> > xyangles;
    for (int xangle = 1; xangle <= LOS_MAX_ANGLE; ++xangle)
        for (int yangle = 1; yangle <= LOS_MAX_ANGLE; ++yangle)
        {
            if (_gcd(xangle, yangle) == 1)
                xyangles.push_back(std::pair<int,int>(xangle, yangle));
        }

    std::sort(xyangles.begin(), xyangles.end(), complexity_lt);
    for (unsigned int i = 0; i < xyangles.size(); ++i)
    {
        const int xangle = xyangles[i].first;
        const int yangle = xyangles[i].second;

        const double slope = ((double)(yangle)) / xangle;
        const double rslope = ((double)(xangle)) / yangle;
        for (int intercept = 1; intercept <= LOS_INTERCEPT_MULT*yangle; ++intercept )
        {
            double xstart = ((double)intercept) / (LOS_INTERCEPT_MULT*yangle);
            double ystart = 1;

            // now move back just inside the cell
            // y should be "about to change"
            xstart -= EPSILON_VALUE * xangle;
            ystart -= EPSILON_VALUE * yangle;

            _register_ray(xstart, ystart, slope);
            // also draw the identical ray in octant 2
            _register_ray(ystart, xstart, rslope);
        }
    }

    // Now create the appropriate blockrays array
    _create_blockrays();
}

static void _set_ray_quadrant(ray_def& ray, int sx, int sy, int tx, int ty)
{
    ray.quadx = tx >= sx ? 1 : -1;
    ray.quady = ty >= sy ? 1 : -1;
}

static const double VERTICAL_SLOPE = 10000.0;
static double _calc_slope(double x, double y)
{
    if (double_is_zero(x))
        return (VERTICAL_SLOPE);

    const double slope = y / x;
    return (slope > VERTICAL_SLOPE ? VERTICAL_SLOPE : slope);
}

static double _slope_factor(const ray_def &ray)
{
    double xdiff = fabs(ray.accx - 0.5), ydiff = fabs(ray.accy - 0.5);

    if (double_is_zero(xdiff) && double_is_zero(ydiff))
        return ray.slope;

    const double slope = _calc_slope(ydiff, xdiff);
    return (slope + ray.slope) / 2.0;
}

static int _imbalance(ray_def ray, const coord_def& target)
{
    int imb = 0;
    int diags = 0, straights = 0;
    while (ray.pos() != target)
    {
        adv_type adv = ray.advance_through(target);
        if (adv == ADV_XY)
        {
            straights = 0;
            if (++diags > imb)
                imb = diags;
            diags++;
        }
        else
        {
            diags = 0;
            if (++straights > imb)
                imb = straights;
        }
    }
    return imb;
}

void cellray::calc_params()
{
    coord_def trg = target();
    imbalance = _imbalance(ray, trg);
    slope_diff = _slope_factor(ray) - _calc_slope(trg.x, trg.y);
}

// Coordinate transformation so we can find_ray quadrant-by-quadrant.
// TODO: Unify with los_params.
struct trans
{
    coord_def source;
    int signx, signy;

    coord_def transform(coord_def l)
    {
        return coord_def(source.x + signx*l.x, source.y + signy*l.y);
    }
};

// Find ray in positive quadrant.
bool _find_ray_se(const coord_def& target, ray_def& ray,
                  bool cycle, bool ignore_solid, trans t)
{
    ASSERT(target.x >= 0 && target.y >= 0 && !target.origin());
    if (target.abs() > LOS_RADIUS2)
        return false;

    const std::vector<cellray> &min = min_cellrays(target);
    ASSERT(min.size() > 0);
    cellray c = min[0]; // XXX: const cellray &c ?
    unsigned int index = 0;

#ifdef DEBUG_DIAGNOSTICS
    if (cycle)
        mprf(MSGCH_DIAGNOSTICS, "cycling from %d (total %d), ignores_solid=%d",
             ray.cycle_idx, min.size(), ignore_solid);
#endif

    unsigned int start = cycle ? ray.cycle_idx + 1 : 0;
    ASSERT(0 <= start && start <= min.size());

    if (ignore_solid)
    {
        index = start % min.size();
        c = min[index];
    }
    else
    {
        bool blocked = true;
        for (unsigned int i = start; blocked && (i < start + min.size()); i++)
        {
            index = i % min.size();
            c = min[index];
            blocked = false;
            // Check all inner points.
            for (unsigned int j = 0; j < c.end && !blocked; j++)
            {
                coord_def cur = t.transform(c[j]);
                blocked = grid_is_solid(grd(cur));
            }
        }
        if (blocked)
            return (false);
    }

    ray = c.ray;
    ray.cycle_idx = index;

    return (true);
}

// Find a nonblocked ray from source to target. Return false if no
// such ray could be found, otherwise return true and fill ray
// appropriately.
// if range is too great or all rays are blocked.
// If cycle is false, find the first fitting ray. If it is true,
// assume that ray is appropriately filled in, and look for the next
// ray.
bool find_ray(const coord_def& source, const coord_def& target,
              ray_def& ray, bool cycle, bool ignore_solid)
{
    if (target == source || !map_bounds(source) || !map_bounds(target))
        return false;

    const int signx = ((target.x - source.x >= 0) ? 1 : -1);
    const int signy = ((target.y - source.y >= 0) ? 1 : -1);
    const int absx  = signx * (target.x - source.x);
    const int absy  = signy * (target.y - source.y);
    const coord_def abs = coord_def(absx, absy);
    trans t;
    t.source = source;
    t.signx = signx;
    t.signy = signy;

    ray.accx -= source.x;
    ray.accy -= source.y;

    if (signx < 0)
        ray.accx = 1.0 - ray.accx;
    if (signy < 0)
        ray.accy = 1.0 - ray.accy;

    ray.quadx = 1;
    ray.quady = 1;

    if (!_find_ray_se(abs, ray, cycle, ignore_solid, t))
        return false;

    if (signx < 0)
        ray.accx = 1.0 - ray.accx;
    if (signy < 0)
        ray.accy = 1.0 - ray.accy;

    ray.accx += source.x;
    ray.accy += source.y;

    _set_ray_quadrant(ray, source.x, source.y, target.x, target.y);

    return true;
}

// Returns a straight ray from source to target.
void fallback_ray(const coord_def& source, const coord_def& target,
                  ray_def& ray)
{
    ray.accx = source.x + 0.5;
    ray.accy = source.y + 0.5;
    coord_def diff = target - source;
    ray.slope = _calc_slope(std::abs(diff.x), std::abs(diff.y));
    _set_ray_quadrant(ray, source.x, source.y, target.x, target.y);
}

// Count the number of matching features between two points along
// a beam-like path; the path will pass through solid features.
// By default, it excludes end points from the count.
// If just_check is true, the function will return early once one
// such feature is encountered.
int num_feats_between(const coord_def& source, const coord_def& target,
                      dungeon_feature_type min_feat,
                      dungeon_feature_type max_feat,
                      bool exclude_endpoints, bool just_check)
{
    ray_def ray;
    int     count    = 0;
    int     max_dist = grid_distance(source, target);

    // We don't need to find the shortest beam, any beam will suffice.
    fallback_ray(source, target, ray);

    if (exclude_endpoints && ray.pos() == source)
    {
        ray.advance(true);
        max_dist--;
    }

    int dist = 0;
    bool reached_target = false;
    while (dist++ <= max_dist)
    {
        const dungeon_feature_type feat = grd(ray.pos());

        if (ray.pos() == target)
            reached_target = true;

        if (feat >= min_feat && feat <= max_feat
            && (!exclude_endpoints || !reached_target))
        {
            count++;

            if (just_check) // Only needs to be > 0.
                return (count);
        }

        if (reached_target)
            break;

        ray.advance(true);
    }

    return (count);
}

// Is p2 visible from p1, disregarding clouds?
// XXX: Horribly inefficient since we do an entire LOS calculation;
//      needs to be rewritten if it is used more.
bool cell_see_cell(const coord_def& p1, const coord_def& p2)
{
    env_show_grid show;
    losight(show, los_param_nocloud(p1));
    return see_grid(show, p1, p2);
}

// We use raycasting. The algorithm:
// PRECOMPUTATION:
// Create a large bundle of rays and cast them.
// Mark, for each one, which cells kill it (and where.)
// Also, for each one, note which cells it passes.
// ACTUAL LOS:
// Unite the ray-killers for the given map; this tells you which rays
// are dead.
// Look up which cells the surviving rays have, and that's your LOS!
// OPTIMIZATIONS:
// WLOG, we can assume that we're in a specific quadrant - say the
// first quadrant - and just mirror everything after that.  We can
// likely get away with a single octant, but we don't do that. (To
// do...)
// Rays are actually split by each cell they pass. So each "ray" only
// identifies a single cell, and we can do logical ORs.  Once a cell
// kills a cellray, it will kill all remaining cellrays of that ray.
// Also, rays are checked to see if they are duplicates of each
// other. If they are, they're eliminated.
// Some cellrays can also be eliminated. In general, a cellray is
// unnecessary if there is another cellray with the same coordinates,
// and whose path (up to those coordinates) is a subset, not necessarily
// proper, of the original path. We still store the original cellrays
// fully for beam detection and such.
// PERFORMANCE:
// With reasonable values we have around 6000 cellrays, meaning
// around 600Kb (75 KB) of data. This gets cut down to 700 cellrays
// after removing duplicates. That means that we need to do
// around 22*100*4 ~ 9,000 memory reads + writes per LOS call on a
// 32-bit system. Not too bad.
// IMPROVEMENTS:
// Smoke will now only block LOS after two cells of smoke. This is
// done by updating with a second array.

void _losight_quadrant(env_show_grid& sh, const los_param& dat, int sx, int sy)
{
    const unsigned int num_cellrays = cellray_ends.size();

    dead_rays->reset();
    smoke_rays->reset();

    for (quadrant_iterator qi; qi; qi++)
    {
        coord_def p = coord_def(sx*(qi->x), sy*(qi->y));
        if (!dat.los_bounds(p))
            continue;

        switch (dat.opacity(p))
        {
        case OPC_OPAQUE:
            // Block the appropriate rays.
            *dead_rays |= *blockrays(*qi);
            break;
        case OPC_HALF:
            // Block rays which have already seen a cloud.
            *dead_rays  |= (*smoke_rays & *blockrays(*qi));
            *smoke_rays |= *blockrays(*qi);
            break;
        default:
            break;
        }
    }

    // Ray calculation done. Now work out which cells in this
    // quadrant are visible.
    for (unsigned int rayidx = 0; rayidx < num_cellrays; ++rayidx)
    {
        // make the cells seen by this ray at this point visible
        if (!dead_rays->get(rayidx))
        {
            // This ray is alive, thus the end cell is visible.
            const coord_def p = coord_def(sx * cellray_ends[rayidx].x,
                                          sy * cellray_ends[rayidx].y);
            if (dat.los_bounds(p))
                sh(p+sh_o) = dat.appearance(p);
        }
    }
}

void losight(env_show_grid& sh, const los_param& dat)
{
    sh.init(0);

    // Do precomputations if necessary.
    raycast();

    const int quadrant_x[4] = {  1, -1, -1,  1 };
    const int quadrant_y[4] = {  1,  1, -1, -1 };
    for (int q = 0; q < 4; ++q)
        _losight_quadrant(sh, dat, quadrant_x[q], quadrant_y[q]);

    // Center is always visible.
    const coord_def o = coord_def(0,0);
    sh(o+sh_o) = dat.appearance(o);
}

void losight_permissive(env_show_grid &sh, const coord_def& center)
{
    for (int x = -ENV_SHOW_OFFSET; x <= ENV_SHOW_OFFSET; ++x)
        for (int y = -ENV_SHOW_OFFSET; y <= ENV_SHOW_OFFSET; ++y)
        {
            const coord_def pos = center + coord_def(x, y);
            if (map_bounds(pos))
                sh[x + sh_xo][y + sh_yo] = env.grid(pos);
        }
}

void calc_show_los()
{
    if (!crawl_state.arena && !crawl_state.arena_suspended)
    {
        // Must be done first.
        losight(env.show, los_param_base(you.pos()));

        // What would be visible, if all of the translucent walls were
        // made opaque.
        losight(env.no_trans_show, los_param_solid(you.pos()));
    }
    else
    {
        losight_permissive(env.show, crawl_view.glosc());
    }
}

bool see_grid(const env_show_grid &show,
              const coord_def &c,
              const coord_def &pos)
{
    if (c == pos)
        return (true);

    const coord_def ip = pos - c;
    if (ip.rdist() < ENV_SHOW_OFFSET)
    {
        const coord_def sp(ip + coord_def(ENV_SHOW_OFFSET, ENV_SHOW_OFFSET));
        if (show(sp))
            return (true);
    }
    return (false);
}

// Answers the question: "Is a grid within character's line of sight?"
bool see_grid(const coord_def &p)
{
    return ((crawl_state.arena || crawl_state.arena_suspended)
                && crawl_view.in_grid_los(p))
            || see_grid(env.show, you.pos(), p);
}

// Answers the question: "Would a grid be within character's line of sight,
// even if all translucent/clear walls were made opaque?"
bool see_grid_no_trans(const coord_def &p)
{
    return see_grid(env.no_trans_show, you.pos(), p);
}

// Is the grid visible, but a translucent wall is in the way?
bool trans_wall_blocking(const coord_def &p)
{
    return see_grid(p) && !see_grid_no_trans(p);
}
