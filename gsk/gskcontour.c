/*
 * Copyright © 2020 Benjamin Otte
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Benjamin Otte <otte@gnome.org>
 */

#include "config.h"

#include "gskcontourprivate.h"

#include "gskcurveprivate.h"
#include "gskpathbuilder.h"
#include "gskpathprivate.h"
#include "gsksplineprivate.h"

typedef struct _GskContourClass GskContourClass;

struct _GskContour
{
  const GskContourClass *klass;
};

struct _GskContourClass
{
  gsize struct_size;
  const char *type_name;

  void                  (* copy)                (const GskContour       *contour,
                                                 GskContour             *dest);
  gsize                 (* get_size)            (const GskContour       *contour);
  GskPathFlags          (* get_flags)           (const GskContour       *contour);
  void                  (* print)               (const GskContour       *contour,
                                                 GString                *string);
  gboolean              (* get_bounds)          (const GskContour       *contour,
                                                 graphene_rect_t        *bounds);
  void                  (* get_start_end)       (const GskContour       *self,
                                                 graphene_point_t       *start,
                                                 graphene_point_t       *end);
  gboolean              (* foreach)             (const GskContour       *contour,
                                                 float                   tolerance,
                                                 GskPathForeachFunc      func,
                                                 gpointer                user_data);
  GskContour *          (* reverse)             (const GskContour       *contour);
  gpointer              (* init_measure)        (const GskContour       *contour,
                                                 float                   tolerance,
                                                 float                  *out_length);
  void                  (* free_measure)        (const GskContour       *contour,
                                                 gpointer                measure_data);
  void                  (* get_point)           (const GskContour       *contour,
                                                 gpointer                measure_data,
                                                 float                   distance,
                                                 GskPathDirection        direction,
                                                 graphene_point_t       *pos,
                                                 graphene_vec2_t        *tangent);
  float                 (* get_curvature)       (const GskContour       *contour,
                                                 gpointer                measure_data,
                                                 float                   distance,
                                                 graphene_point_t       *center);
  gboolean              (* get_closest_point)   (const GskContour       *contour,
                                                 gpointer                measure_data,
                                                 float                   tolerance,
                                                 const graphene_point_t *point,
                                                 float                   threshold,
                                                 float                  *out_offset,
                                                 graphene_point_t       *out_pos,
                                                 float                  *out_distance,
                                                 graphene_vec2_t        *out_tangent);
  void                  (* add_segment)         (const GskContour       *contour,
                                                 GskPathBuilder         *builder,
                                                 gpointer                measure_data,
                                                 gboolean                emit_move_to,
                                                 float                   start,
                                                 float                   end);
  int                   (* get_winding)         (const GskContour       *contour,
                                                 gpointer                measure_data,
                                                 const graphene_point_t *point);
};

static gsize
gsk_contour_get_size_default (const GskContour *contour)
{
  return contour->klass->struct_size;
}

/* {{{ Utilities */

static void
_g_string_append_double (GString *string,
                         double   d)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_dtostr (buf, G_ASCII_DTOSTR_BUF_SIZE, d);
  g_string_append (string, buf);
}

static void
_g_string_append_point (GString                *string,
                        const graphene_point_t *pt)
{
  _g_string_append_double (string, pt->x);
  g_string_append_c (string, ' ');
  _g_string_append_double (string, pt->y);
}

static void
gsk_find_point_on_line (const graphene_point_t *a,
                        const graphene_point_t *b,
                        const graphene_point_t *p,
                        float                  *offset,
                        graphene_point_t       *pos)
{
  graphene_vec2_t n;
  graphene_vec2_t ap;
  float t;

  graphene_vec2_init (&n, b->x - a->x, b->y - a->y);
  graphene_vec2_init (&ap, p->x - a->x, p->y - a->y);

  t = graphene_vec2_dot (&ap, &n) / graphene_vec2_dot (&n, &n);

  if (t <= 0)
    {
      *pos = *a;
      *offset = 0;
    }
  else if (t >= 1)
    {
      *pos = *b;
      *offset = 1;
    }
  else
    {
      graphene_point_interpolate (a, b, t, pos);
      *offset = t;
    }
}

/* }}} */
/* {{{ Standard */

typedef struct _GskStandardContour GskStandardContour;
struct _GskStandardContour
{
  GskContour contour;

  GskPathFlags flags;

  gsize n_ops;
  gsize n_points;
  graphene_point_t *points;
  gskpathop ops[];
};

static gsize
gsk_standard_contour_compute_size (gsize n_ops,
                                   gsize n_points)
{
  return sizeof (GskStandardContour)
       + sizeof (gskpathop) * n_ops
       + sizeof (graphene_point_t) * n_points;
}

static void
gsk_standard_contour_init (GskContour             *contour,
                           GskPathFlags            flags,
                           const graphene_point_t *points,
                           gsize                   n_points,
                           const gskpathop        *ops,
                           gsize                   n_ops,
                           ptrdiff_t               offset);

static void
gsk_standard_contour_copy (const GskContour *contour,
                           GskContour       *dest)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;

  gsk_standard_contour_init (dest, self->flags, self->points, self->n_points, self->ops, self->n_ops, 0);
}

static gsize
gsk_standard_contour_get_size (const GskContour *contour)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;

  return gsk_standard_contour_compute_size (self->n_ops, self->n_points);
}

static gboolean
gsk_standard_contour_foreach (const GskContour   *contour,
                              float               tolerance,
                              GskPathForeachFunc  func,
                              gpointer            user_data)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;
  gsize i;

  for (i = 0; i < self->n_ops; i ++)
    {
      if (!gsk_pathop_foreach (self->ops[i], func, user_data))
        return FALSE;
    }

  return TRUE;
}

static gboolean
add_reverse (GskPathOperation        op,
             const graphene_point_t *pts,
             gsize                   n_pts,
             float                   weight,
             gpointer                user_data)
{
  GskPathBuilder *builder = user_data;
  GskCurve c, r;

  if (op == GSK_PATH_MOVE)
    return TRUE;

  if (op == GSK_PATH_CLOSE)
    op = GSK_PATH_LINE;

  gsk_curve_init_foreach (&c, op, pts, n_pts, weight);
  gsk_curve_reverse (&c, &r);
  gsk_curve_builder_to (&r, builder);

  return TRUE;
}

static GskContour *
gsk_standard_contour_reverse (const GskContour *contour)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;
  GskPathBuilder *builder;
  GskPath *path;
  GskContour *res;

  builder = gsk_path_builder_new ();

  gsk_path_builder_move_to (builder, self->points[self->n_points - 1].x,
                                     self->points[self->n_points - 1].y);

  for (int i = self->n_ops - 1; i >= 0; i--)
    gsk_pathop_foreach (self->ops[i], add_reverse, builder);

  if (self->flags & GSK_PATH_CLOSED)
    gsk_path_builder_close (builder);

  path = gsk_path_builder_free_to_path (builder);

  g_assert (gsk_path_get_n_contours (path) == 1);

  res = gsk_contour_dup (gsk_path_get_contour (path, 0));

  gsk_path_unref (path);

  return res;
}

static GskPathFlags
gsk_standard_contour_get_flags (const GskContour *contour)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;

  return self->flags;
}

static void
gsk_standard_contour_print (const GskContour *contour,
                            GString          *string)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;
  gsize i;

  for (i = 0; i < self->n_ops; i ++)
    {
      const graphene_point_t *pt = gsk_pathop_points (self->ops[i]);

      switch (gsk_pathop_op (self->ops[i]))
      {
        case GSK_PATH_MOVE:
          g_string_append (string, "M ");
          _g_string_append_point (string, &pt[0]);
          break;

        case GSK_PATH_CLOSE:
          g_string_append (string, " Z");
          break;

        case GSK_PATH_LINE:
          g_string_append (string, " L ");
          _g_string_append_point (string, &pt[1]);
          break;

        case GSK_PATH_QUAD:
          g_string_append (string, " Q ");
          _g_string_append_point (string, &pt[1]);
          g_string_append (string, ", ");
          _g_string_append_point (string, &pt[2]);
          break;

        case GSK_PATH_CUBIC:
          g_string_append (string, " C ");
          _g_string_append_point (string, &pt[1]);
          g_string_append (string, ", ");
          _g_string_append_point (string, &pt[2]);
          g_string_append (string, ", ");
          _g_string_append_point (string, &pt[3]);
          break;

        case GSK_PATH_CONIC:
          /* This is not valid SVG */
          g_string_append (string, " O ");
          _g_string_append_point (string, &pt[1]);
          g_string_append (string, ", ");
          _g_string_append_point (string, &pt[3]);
          g_string_append (string, ", ");
          _g_string_append_double (string, pt[2].x);
          break;

        default:
          g_assert_not_reached();
          return;
      }
    }
}

static void
rect_add_point (graphene_rect_t        *rect,
                const graphene_point_t *point)
{
  if (point->x < rect->origin.x)
    {
      rect->size.width += rect->origin.x - point->x;
      rect->origin.x = point->x;
    }
  else if (point->x > rect->origin.x + rect->size.width)
    {
      rect->size.width = point->x - rect->origin.x;
    }

  if (point->y < rect->origin.y)
    {
      rect->size.height += rect->origin.y - point->y;
      rect->origin.y = point->y;
    }
  else if (point->y > rect->origin.y + rect->size.height)
    {
      rect->size.height = point->y - rect->origin.y;
    }
}

static gboolean
gsk_standard_contour_get_bounds (const GskContour *contour,
                                 graphene_rect_t  *bounds)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;
  gsize i;

  if (self->n_points == 0)
    return FALSE;

  graphene_rect_init (bounds,
                      self->points[0].x, self->points[0].y,
                      0, 0);

  for (i = 1; i < self->n_points; i ++)
    {
      rect_add_point (bounds, &self->points[i]);
    }

  return bounds->size.width > 0 && bounds->size.height > 0;
}

static void
gsk_standard_contour_get_start_end (const GskContour *contour,
                                    graphene_point_t *start,
                                    graphene_point_t *end)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;

  if (start)
    *start = self->points[0];

  if (end)
    *end = self->points[self->n_points - 1];
}

typedef struct
{
  float start;
  float end;
  float start_progress;
  float end_progress;
  GskCurveLineReason reason;
  graphene_point_t start_point;
  graphene_point_t end_point;
  gsize op;
} GskStandardContourMeasure;

typedef struct
{
  GArray *array;
  GskStandardContourMeasure measure;
} LengthDecompose;

static void
gsk_standard_contour_measure_get_point_at (GskStandardContourMeasure *measure,
                                           float                      progress,
                                           graphene_point_t          *out_point)
{
  graphene_point_interpolate (&measure->start_point,
                              &measure->end_point,
                              (progress - measure->start) / (measure->end - measure->start),
                              out_point);
}

static gboolean
gsk_standard_contour_measure_add_point (const graphene_point_t *from,
                                        const graphene_point_t *to,
                                        float                   from_progress,
                                        float                   to_progress,
                                        GskCurveLineReason      reason,
                                        gpointer                user_data)
{
  LengthDecompose *decomp = user_data;
  float seg_length;

  seg_length = graphene_point_distance (from, to, NULL, NULL);
  if (seg_length == 0)
    return TRUE;

  decomp->measure.end += seg_length;
  decomp->measure.start_progress = from_progress;
  decomp->measure.end_progress = to_progress;
  decomp->measure.start_point = *from;
  decomp->measure.end_point = *to;
  decomp->measure.reason = reason;

  g_array_append_val (decomp->array, decomp->measure);

  decomp->measure.start += seg_length;

  return TRUE;
}

static gpointer
gsk_standard_contour_init_measure (const GskContour *contour,
                                   float             tolerance,
                                   float            *out_length)
{
  const GskStandardContour *self = (const GskStandardContour *) contour;
  gsize i;
  float length;
  GArray *array;

  array = g_array_new (FALSE, FALSE, sizeof (GskStandardContourMeasure));
  length = 0;

  for (i = 1; i < self->n_ops; i ++)
    {
      GskCurve curve;
      LengthDecompose decomp = { array, { length, length, 0, 0, GSK_CURVE_LINE_REASON_SHORT, { 0, 0 }, { 0, 0 }, i } };

      gsk_curve_init (&curve, self->ops[i]);
      gsk_curve_decompose (&curve, tolerance, gsk_standard_contour_measure_add_point, &decomp);
      length = decomp.measure.start;
    }

  *out_length = length;

  return array;
}

static void
gsk_standard_contour_free_measure (const GskContour *contour,
                                   gpointer          data)
{
  g_array_free (data, TRUE);
}

static int
gsk_standard_contour_find_measure (gconstpointer m,
                                   gconstpointer l)
{
  const GskStandardContourMeasure *measure = m;
  float length = *(const float *) l;

  if (measure->start > length)
    return 1;
  else if (measure->end <= length)
    return -1;
  else
    return 0;
}

static void
gsk_standard_contour_get_point (const GskContour *contour,
                                gpointer          measure_data,
                                float             distance,
                                GskPathDirection  direction,
                                graphene_point_t *pos,
                                graphene_vec2_t  *tangent)
{
  GskStandardContour *self = (GskStandardContour *) contour;
  GArray *array = measure_data;
  guint index;
  float progress;
  GskStandardContourMeasure *measure;
  GskCurve curve;

  if (array->len == 0)
    {
      g_assert (distance == 0);
      g_assert (gsk_pathop_op (self->ops[0]) == GSK_PATH_MOVE);
      if (pos)
        *pos = self->points[0];
      if (tangent)
        graphene_vec2_init (tangent, 1.f, 0.f);
      return;
    }

  if (!g_array_binary_search (array, &distance, gsk_standard_contour_find_measure, &index))
    index = array->len - 1;

  measure = &g_array_index (array, GskStandardContourMeasure, index);
  progress = (distance - measure->start) / (measure->end - measure->start);

  if (distance == measure->start && direction == GSK_PATH_START &&
      index > 0)
    {
      measure = &g_array_index (array, GskStandardContourMeasure, index - 1);
      progress = 1.f;
    }
  else if (distance == measure->start && direction == GSK_PATH_START &&
           index == 0 &&
           (self->flags & GSK_PATH_CLOSED) != 0)
    {
      measure = &g_array_index (array, GskStandardContourMeasure, array->len - 1);
      progress = 1.f;
    }

  progress = measure->start_progress + (measure->end_progress - measure->start_progress) * progress;
  g_assert (progress >= 0 && progress <= 1);

  gsk_curve_init (&curve, self->ops[measure->op]);

  if (pos)
    gsk_curve_get_point (&curve, progress, pos);
  if (tangent)
    gsk_curve_get_tangent (&curve, progress, tangent);
}

static float
gsk_standard_contour_get_curvature (const GskContour *contour,
                                    gpointer          measure_data,
                                    float             distance,
                                    graphene_point_t *center)
{
  GskStandardContour *self = (GskStandardContour *) contour;
  GArray *array = measure_data;
  guint index;
  float progress;
  GskStandardContourMeasure *measure;
  GskCurve curve;

  if (array->len == 0)
    {
      g_assert (distance == 0);
      g_assert (gsk_pathop_op (self->ops[0]) == GSK_PATH_MOVE);
      return 0;
    }

  if (!g_array_binary_search (array, &distance, gsk_standard_contour_find_measure, &index))
    index = array->len - 1;
  measure = &g_array_index (array, GskStandardContourMeasure, index);
  progress = (distance - measure->start) / (measure->end - measure->start);
  progress = measure->start_progress + (measure->end_progress - measure->start_progress) * progress;
  g_assert (progress >= 0 && progress <= 1);

  gsk_curve_init (&curve, self->ops[measure->op]);

  return gsk_curve_get_curvature (&curve, progress, center);
}

static gboolean
gsk_standard_contour_get_closest_point (const GskContour       *contour,
                                        gpointer                measure_data,
                                        float                   tolerance,
                                        const graphene_point_t *point,
                                        float                   threshold,
                                        float                  *out_distance,
                                        graphene_point_t       *out_pos,
                                        float                  *out_offset,
                                        graphene_vec2_t        *out_tangent)
{
  GskStandardContour *self = (GskStandardContour *) contour;
  GskStandardContourMeasure *measure;
  float progress, dist;
  GArray *array = measure_data;
  graphene_point_t p, last_point;
  gsize i;
  gboolean result = FALSE;

  g_assert (gsk_pathop_op (self->ops[0]) == GSK_PATH_MOVE);
  last_point = self->points[0];

  if (array->len == 0)
    {
      /* This is the special case for point-only */
      dist = graphene_point_distance (&last_point, point, NULL, NULL);

      if (dist > threshold)
        return FALSE;

      if (out_offset)
        *out_offset = 0;

      if (out_distance)
        *out_distance = dist;

      if (out_pos)
        *out_pos = last_point;

      if (out_tangent)
        *out_tangent = *graphene_vec2_x_axis ();

      return TRUE;
    }

  for (i = 0; i < array->len; i++)
    {
      measure = &g_array_index (array, GskStandardContourMeasure, i);

      gsk_find_point_on_line (&last_point,
                              &measure->end_point,
                              point,
                              &progress,
                              &p);
      last_point = measure->end_point;
      dist = graphene_point_distance (point, &p, NULL, NULL);
      /* add some wiggleroom for the accurate check below */
      //g_print ("%zu: (%g-%g) dist %g\n", i, measure->start, measure->end, dist);
      if (dist <= threshold + 1.0f)
        {
          GskCurve curve;
          graphene_point_t p2;
          float found_progress, test_progress, test_dist;
          const float step = 1/1024.f;

          gsk_curve_init (&curve, self->ops[measure->op]);

          found_progress = measure->start_progress + (measure->end_progress - measure->start_progress) * progress;
          gsk_curve_get_point (&curve, found_progress, &p);
          dist = graphene_point_distance (point, &p, NULL, NULL);
          //g_print ("!!! %zu: (%g-%g @ %g) dist %g\n", i, measure->start_progress, measure->end_progress, progress, dist);

          /* The progress is non-uniform, so simple translation of progress doesn't work.
           * Check if larger values inch closer towards minimal distance. */
          while (progress + step < 1.0f) {
            test_progress = measure->start_progress + (measure->end_progress - measure->start_progress) * (progress + step);
            gsk_curve_get_point (&curve, test_progress, &p2);
            test_dist = graphene_point_distance (point, &p2, NULL, NULL);
            if (test_dist > dist)
              break;
            progress += step;
            p = p2;
            found_progress = test_progress;
            dist = test_dist;
          }
          /* Also check smaller ones */
          while (progress - step > 0.0f) {
            test_progress = measure->start_progress + (measure->end_progress - measure->start_progress) * (progress - step);
            gsk_curve_get_point (&curve, test_progress, &p2);
            test_dist = graphene_point_distance (point, &p2, NULL, NULL);
            if (test_dist > dist)
              break;
            progress -= step;
            p = p2;
            found_progress = test_progress;
            dist = test_dist;
          }
          //g_print ("!!! %zu: (%g-%g @ %g) dist %g\n", i, measure->start_progress, measure->end_progress, progress, dist);
          /* double check that the point actually is closer */ 
          if (dist <= threshold)
            {
              if (out_distance)
                *out_distance = dist;
              if (out_pos)
                *out_pos = p;
              if (out_offset)
                *out_offset = measure->start + (measure->end - measure->start) * progress;
              if (out_tangent)
                gsk_curve_get_tangent (&curve, found_progress, out_tangent);
              result = TRUE;
              if (tolerance >= dist)
                  return TRUE;
              threshold = dist - tolerance;
            }
        }
    }

  return result;
}

static void
gsk_standard_contour_add_segment (const GskContour *contour,
                                  GskPathBuilder   *builder,
                                  gpointer          measure_data,
                                  gboolean          emit_move_to,
                                  float             start,
                                  float             end)
{
  GskStandardContour *self = (GskStandardContour *) contour;
  GArray *array = measure_data;
  guint start_index, end_index;
  float start_progress, end_progress;
  GskStandardContourMeasure *start_measure, *end_measure;
  gsize i;

  if (start > 0)
    {
      if (!g_array_binary_search (array, (float[1]) { start }, gsk_standard_contour_find_measure, &start_index))
        start_index = array->len - 1;
      start_measure = &g_array_index (array, GskStandardContourMeasure, start_index);
      start_progress = (start - start_measure->start) / (start_measure->end - start_measure->start);
      start_progress = start_measure->start_progress + (start_measure->end_progress - start_measure->start_progress) * start_progress;
      g_assert (start_progress >= 0 && start_progress <= 1);
    }
  else
    {
      start_measure = NULL;
      start_progress = 0.0;
    }

  if (g_array_binary_search (array, (float[1]) { end }, gsk_standard_contour_find_measure, &end_index))
    {
      end_measure = &g_array_index (array, GskStandardContourMeasure, end_index);
      end_progress = (end - end_measure->start) / (end_measure->end - end_measure->start);
      end_progress = end_measure->start_progress + (end_measure->end_progress - end_measure->start_progress) * end_progress;
      g_assert (end_progress >= 0 && end_progress <= 1);
    }
  else
    {
      end_measure = NULL;
      end_progress = 1.0;
    }

  /* Add the first partial operation,
   * taking care that first and last operation might be identical */
  if (start_measure)
    {
      GskCurve curve, cut;
      const graphene_point_t *start_point;

      gsk_curve_init (&curve, self->ops[start_measure->op]);

      if (start_measure->reason == GSK_CURVE_LINE_REASON_STRAIGHT)
        {
          graphene_point_t p;

          gsk_standard_contour_measure_get_point_at (start_measure, start, &p);
          if (emit_move_to)
            gsk_path_builder_move_to (builder, p.x, p.y);

          if (end_measure == start_measure)
            {
              gsk_standard_contour_measure_get_point_at (end_measure, end, &p);
              gsk_path_builder_line_to (builder, p.x, p.y);
              return;
            }
          else
            {
              gsk_path_builder_line_to (builder,
                                        start_measure->end_point.x,
                                        start_measure->end_point.y);
              start_index++;
              if (start_index >= array->len)
                return;

              start_measure++;
              start_progress = start_measure->start_progress;
              emit_move_to = FALSE;
              gsk_curve_init (&curve, self->ops[start_measure->op]);
            }
        }

      if (end_measure && end_measure->op == start_measure->op)
        {
          if (end_measure->reason == GSK_CURVE_LINE_REASON_SHORT)
            {
              gsk_curve_segment (&curve, start_progress, end_progress, &cut);
              if (emit_move_to)
                {
                  start_point = gsk_curve_get_start_point (&cut);
                  gsk_path_builder_move_to (builder, start_point->x, start_point->y);
                }
              gsk_curve_builder_to (&cut, builder);
            }
          else
            {
              graphene_point_t p;

              gsk_curve_segment (&curve, start_progress, end_measure->start_progress, &cut);
              if (emit_move_to)
                {
                  start_point = gsk_curve_get_start_point (&cut);
                  gsk_path_builder_move_to (builder, start_point->x, start_point->y);
                }
              gsk_curve_builder_to (&cut, builder);

              gsk_standard_contour_measure_get_point_at (end_measure, end, &p);
              gsk_path_builder_line_to (builder, p.x, p.y);
            }
          return;
        }

      gsk_curve_split (&curve, start_progress, NULL, &cut);

      start_point = gsk_curve_get_start_point (&cut);
      if (emit_move_to)
        gsk_path_builder_move_to (builder, start_point->x, start_point->y);
      gsk_curve_builder_to (&cut, builder);
      i = start_measure->op + 1;
    }
  else 
    i = emit_move_to ? 0 : 1;

  for (; i < (end_measure ? end_measure->op : self->n_ops - 1); i++)
    {
      gsk_path_builder_pathop_to (builder, self->ops[i]);
    }

  /* Add the last partial operation */
  if (end_measure)
    {
      GskCurve curve, cut;

      gsk_curve_init (&curve, self->ops[end_measure->op]);

      if (end_measure->reason == GSK_CURVE_LINE_REASON_SHORT)
        {
          gsk_curve_split (&curve, end_progress, &cut, NULL);
          gsk_curve_builder_to (&cut, builder);
        }
      else
        {
          graphene_point_t p;
          gsk_curve_split (&curve, end_measure->start_progress, &cut, NULL);
          gsk_curve_builder_to (&cut, builder);

          gsk_standard_contour_measure_get_point_at (end_measure, end, &p);
          gsk_path_builder_line_to (builder, p.x, p.y);
        }
    }
  else if (i == self->n_ops - 1)
    {
      gskpathop op = self->ops[i];
      if (gsk_pathop_op (op) == GSK_PATH_CLOSE)
        gsk_path_builder_pathop_to (builder, gsk_pathop_encode (GSK_PATH_LINE, gsk_pathop_points (op)));
      else
        gsk_path_builder_pathop_to (builder, op);
    }
}

static inline int
line_get_crossing (const graphene_point_t *p,
                   const graphene_point_t *p1,
                   const graphene_point_t *p2)
{
  if (p1->y <= p->y)
    {
      if (p2->y > p->y)
        {
          if ((p2->x - p1->x) * (p->y - p1->y) - (p->x - p1->x) * (p2->y - p1->y) > 0)
            return 1;
        }
    }
  else if (p2->y <= p->y)
    {
      if ((p2->x - p1->x) * (p->y - p1->y) - (p->x - p1->x) * (p2->y - p1->y) < 0)
        return -1;
    }

  return 0;
}

static int
gsk_standard_contour_get_winding (const GskContour       *contour,
                                  gpointer                measure_data,
                                  const graphene_point_t *point)
{
  GskStandardContour *self = (GskStandardContour *) contour;
  GArray *array = measure_data;
  graphene_point_t last_point;
  int winding;
  int i;

  if (array->len == 0)
    return 0;

  winding = 0;
  last_point = self->points[0];
  for (i = 0; i < array->len; i++)
    {
      GskStandardContourMeasure *measure;

      measure = &g_array_index (array, GskStandardContourMeasure, i);
      winding += line_get_crossing (point, &last_point, &measure->end_point);

      last_point = measure->end_point;
    }

  winding += line_get_crossing (point, &last_point, &self->points[0]);

  return winding;
}

static const GskContourClass GSK_STANDARD_CONTOUR_CLASS =
{
  sizeof (GskStandardContour),
  "GskStandardContour",
  gsk_standard_contour_copy,
  gsk_standard_contour_get_size,
  gsk_standard_contour_get_flags,
  gsk_standard_contour_print,
  gsk_standard_contour_get_bounds,
  gsk_standard_contour_get_start_end,
  gsk_standard_contour_foreach,
  gsk_standard_contour_reverse,
  gsk_standard_contour_init_measure,
  gsk_standard_contour_free_measure,
  gsk_standard_contour_get_point,
  gsk_standard_contour_get_curvature,
  gsk_standard_contour_get_closest_point,
  gsk_standard_contour_add_segment,
  gsk_standard_contour_get_winding,
};

/* You must ensure the contour has enough size allocated,
 * see gsk_standard_contour_compute_size()
 */
static void
gsk_standard_contour_init (GskContour             *contour,
                           GskPathFlags            flags,
                           const graphene_point_t *points,
                           gsize                   n_points,
                           const gskpathop        *ops,
                           gsize                   n_ops,
                           gssize                  offset)

{
  GskStandardContour *self = (GskStandardContour *) contour;
  gsize i;

  self->contour.klass = &GSK_STANDARD_CONTOUR_CLASS;

  self->flags = flags;
  self->n_ops = n_ops;
  self->n_points = n_points;
  self->points = (graphene_point_t *) &self->ops[n_ops];
  memcpy (self->points, points, sizeof (graphene_point_t) * n_points);

  offset += self->points - points;
  for (i = 0; i < n_ops; i++)
    {
      self->ops[i] = gsk_pathop_encode (gsk_pathop_op (ops[i]),
                                        gsk_pathop_points (ops[i]) + offset);
    }
}

GskContour *
gsk_standard_contour_new (GskPathFlags            flags,
                          const graphene_point_t *points,
                          gsize                   n_points,
                          const gskpathop        *ops,
                          gsize                   n_ops,
                          gssize                  offset)
{
  GskContour *contour;

  contour = g_malloc0 (gsk_standard_contour_compute_size (n_ops, n_points));

  gsk_standard_contour_init (contour, flags, points, n_points, ops, n_ops, offset);

  return contour;
}

/* }}} */
/* {{{ Rectangle */

typedef struct _GskRectContour GskRectContour;
struct _GskRectContour
{
  GskContour contour;

  float x;
  float y;
  float width;
  float height;
};

static void
gsk_rect_contour_copy (const GskContour *contour,
                       GskContour       *dest)
{
  const GskRectContour *self = (const GskRectContour *) contour;
  GskRectContour *target = (GskRectContour *) dest;

  *target = *self;
}

static GskPathFlags
gsk_rect_contour_get_flags (const GskContour *contour)
{
  return GSK_PATH_FLAT | GSK_PATH_CLOSED;
}

static void
gsk_rect_contour_print (const GskContour *contour,
                        GString          *string)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  g_string_append (string, "M ");
  _g_string_append_point (string, &GRAPHENE_POINT_INIT (self->x, self->y));
  g_string_append (string, " h ");
  _g_string_append_double (string, self->width);
  g_string_append (string, " v ");
  _g_string_append_double (string, self->height);
  g_string_append (string, " h ");
  _g_string_append_double (string, - self->width);
  g_string_append (string, " z");
}

static gboolean
gsk_rect_contour_get_bounds (const GskContour *contour,
                             graphene_rect_t  *rect)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  graphene_rect_init (rect, self->x, self->y, self->width, self->height);

  return TRUE;
}

static void
gsk_rect_contour_get_start_end (const GskContour *contour,
                                graphene_point_t *start,
                                graphene_point_t *end)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  if (start)
    *start = GRAPHENE_POINT_INIT (self->x, self->y);

  if (end)
    *end = GRAPHENE_POINT_INIT (self->x, self->y);
}

static gboolean
gsk_rect_contour_foreach (const GskContour   *contour,
                          float               tolerance,
                          GskPathForeachFunc  func,
                          gpointer            user_data)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  graphene_point_t pts[] = {
    GRAPHENE_POINT_INIT (self->x,               self->y),
    GRAPHENE_POINT_INIT (self->x + self->width, self->y),
    GRAPHENE_POINT_INIT (self->x + self->width, self->y + self->height),
    GRAPHENE_POINT_INIT (self->x,               self->y + self->height),
    GRAPHENE_POINT_INIT (self->x,               self->y)
  };

  return func (GSK_PATH_MOVE, &pts[0], 1, 0, user_data)
      && func (GSK_PATH_LINE, &pts[0], 2, 0, user_data)
      && func (GSK_PATH_LINE, &pts[1], 2, 0, user_data)
      && func (GSK_PATH_LINE, &pts[2], 2, 0, user_data)
      && func (GSK_PATH_CLOSE, &pts[3], 2, 0, user_data);
}

static GskContour *
gsk_rect_contour_reverse (const GskContour *contour)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  return gsk_rect_contour_new (&GRAPHENE_RECT_INIT (self->x + self->width,
                                                    self->y,
                                                    - self->width,
                                                    self->height));
}

static gpointer
gsk_rect_contour_init_measure (const GskContour *contour,
                               float             tolerance,
                               float            *out_length)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  *out_length = 2 * ABS (self->width) + 2 * ABS (self->height);

  return NULL;
}

static void
gsk_rect_contour_free_measure (const GskContour *contour,
                               gpointer          data)
{
}

static void
gsk_rect_contour_get_point (const GskContour *contour,
                            gpointer          measure_data,
                            float             distance,
                            GskPathDirection  direction,
                            graphene_point_t *pos,
                            graphene_vec2_t  *tangent)
{
  const GskRectContour *self = (const GskRectContour *) contour;

  if (distance == 0)
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x, self->y);

      if (tangent)
        {
          if (direction == GSK_PATH_START)
            graphene_vec2_init (tangent, 0.0f, - copysignf (1.0f, self->height));
          else
            graphene_vec2_init (tangent, copysignf (1.0f, self->width), 0.0f);
        }
      return;
    }

  if (distance < fabsf (self->width))
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x + copysignf (distance, self->width), self->y);
      if (tangent)
        graphene_vec2_init (tangent, copysignf (1.0f, self->width), 0.0f);
      return;
    }
  distance -= fabsf (self->width);

  if (distance == 0)
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x + self->width, self->y);

      if (tangent)
        {
          if (direction == GSK_PATH_START)
            graphene_vec2_init (tangent, copysignf (1.0f, self->width), 0.0f);
          else
            graphene_vec2_init (tangent, 0.0f, copysignf (1.0f, self->height));
        }
      return;
    }

  if (distance < fabsf (self->height))
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x + self->width, self->y + copysignf (distance, self->height));
      if (tangent)
        graphene_vec2_init (tangent, 0.0f, copysignf (1.0f, self->height));
      return;
    }
  distance -= fabs (self->height);

  if (distance == 0)
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x + self->width, self->y + self->height);

      if (tangent)
        {
          if (direction == GSK_PATH_START)
            graphene_vec2_init (tangent, 0.0f, copysignf (1.0f, self->height));
          else
            graphene_vec2_init (tangent, - copysignf (1.0f, self->width), 0.0f);
        }
      return;
    }

  if (distance < fabsf (self->width))
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x + self->width - copysignf (distance, self->width), self->y + self->height);
      if (tangent)
        graphene_vec2_init (tangent, - copysignf (1.0f, self->width), 0.0f);
      return;
    }
  distance -= fabsf (self->width);

  if (distance == 0)
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x, self->y + self->height);

      if (tangent)
        {
          if (direction == GSK_PATH_START)
            graphene_vec2_init (tangent, - copysignf (1.0f, self->width), 0.0f);
          else
            graphene_vec2_init (tangent, 0.0f, - copysignf (1.0f, self->height));
        }
      return;
    }

  if (distance < fabsf (self->height))
    {
      if (pos)
        *pos = GRAPHENE_POINT_INIT (self->x, self->y + self->height - copysignf (distance, self->height));
      if (tangent)
        graphene_vec2_init (tangent, 0.0f, - copysignf (1.0f, self->height));
      return;
    }

  if (pos)
    *pos = GRAPHENE_POINT_INIT (self->x, self->y);

  if (tangent)
    {
      if (direction == GSK_PATH_START)
        graphene_vec2_init (tangent, 0.0f, - copysignf (1.0f, self->height));
      else
        graphene_vec2_init (tangent, copysignf (1.0f, self->width), 0.0f);
    }
}

static float
gsk_rect_contour_get_curvature (const GskContour *contour,
                                gpointer          measure_data,
                                float             distance,
                                graphene_point_t *center)
{
  return 0;
}

static gboolean
gsk_rect_contour_get_closest_point (const GskContour       *contour,
                                    gpointer                measure_data,
                                    float                   tolerance,
                                    const graphene_point_t *point,
                                    float                   threshold,
                                    float                  *out_distance,
                                    graphene_point_t       *out_pos,
                                    float                  *out_offset,
                                    graphene_vec2_t        *out_tangent)
{
  const GskRectContour *self = (const GskRectContour *) contour;
  graphene_point_t t, p;
  float distance;

  /* offset coords to be relative to rectangle */
  t.x = point->x - self->x;
  t.y = point->y - self->y;

  if (self->width)
    {
      /* do unit square math */
      t.x /= self->width;
      /* move point onto the square */
      t.x = CLAMP (t.x, 0.f, 1.f);
    }
  else
    t.x = 0.f;

  if (self->height)
    {
      t.y /= self->height;
      t.y = CLAMP (t.y, 0.f, 1.f);
    }
  else
    t.y = 0.f;

  if (t.x > 0 && t.x < 1 && t.y > 0 && t.y < 1)
    {
      float diff = MIN (t.x, 1.f - t.x) * ABS (self->width) - MIN (t.y, 1.f - t.y) * ABS (self->height);

      if (diff < 0.f)
        t.x = ceilf (t.x - 0.5f); /* round 0.5 down */
      else if (diff > 0.f)
        t.y = roundf (t.y); /* round 0.5 up */
      else
        {
          /* at least 2 points match, return the first one in the stroke */
          if (t.y <= 1.f - t.y)
            t.y = 0.f;
          else if (1.f - t.x <= t.x)
            t.x = 1.f;
          else
            t.y = 1.f;
        }
    }

  /* Don't let -0 confuse us */
  t.x = fabs (t.x);
  t.y = fabs (t.y);

  p = GRAPHENE_POINT_INIT (self->x + t.x * self->width,
                           self->y + t.y * self->height);

  distance = graphene_point_distance (point, &p, NULL, NULL);
  if (distance > threshold)
    return FALSE;

  if (out_distance)
    *out_distance = distance;

  if (out_pos)
    *out_pos = p;

  if (out_offset)
    *out_offset = ((t.x == 0.0 && (t.y > 0 && self->width != 0)) ? 2 - t.y : t.y) * ABS (self->height) +
                  ((t.y == 1.0 || (t.y > 0.0 && t.x == 0.0)) ? 2 - t.x : t.x) * ABS (self->width);

  if (out_tangent)
    {
      if (t.y == 0 && t.x < 1)
        graphene_vec2_init (out_tangent, copysignf(1.0, self->width), 0);
      else if (t.x == 0)
        graphene_vec2_init (out_tangent, 0, - copysignf(1.0, self->height));
      else if (t.y == 1)
        graphene_vec2_init (out_tangent, - copysignf(1.0, self->width), 0);
      else if (t.x == 1)
        graphene_vec2_init (out_tangent, 0, copysignf(1.0, self->height));
    }

  return TRUE;
}

static void
gsk_rect_contour_add_segment (const GskContour *contour,
                              GskPathBuilder   *builder,
                              gpointer          measure_data,
                              gboolean          emit_move_to,
                              float             start,
                              float             end)
{
  const GskRectContour *self = (const GskRectContour *) contour;
  float w = ABS (self->width);
  float h = ABS (self->height);

  if (start < w)
    {
      if (emit_move_to)
        gsk_path_builder_move_to (builder, self->x + start * (w / self->width), self->y);
      if (end <= w)
        {
          gsk_path_builder_line_to (builder, self->x + end * (w / self->width), self->y);
          return;
        }
      gsk_path_builder_line_to (builder, self->x + self->width, self->y);
    }
  start -= w;
  end -= w;

  if (start < h)
    {
      if (start >= 0 && emit_move_to)
        gsk_path_builder_move_to (builder, self->x + self->width, self->y + start * (h / self->height));
      if (end <= h)
        {
          gsk_path_builder_line_to (builder, self->x + self->width, self->y + end * (h / self->height));
          return;
        }
      gsk_path_builder_line_to (builder, self->x + self->width, self->y + self->height);
    }
  start -= h;
  end -= h;

  if (start < w)
    {
      if (start >= 0 && emit_move_to)
        gsk_path_builder_move_to (builder, self->x + (w - start) * (w / self->width), self->y + self->height);
      if (end <= w)
        {
          gsk_path_builder_line_to (builder, self->x + (w - end) * (w / self->width), self->y + self->height);
          return;
        }
      gsk_path_builder_line_to (builder, self->x, self->y + self->height);
    }
  start -= w;
  end -= w;

  if (start < h)
    {
      if (start >= 0 && emit_move_to)
        gsk_path_builder_move_to (builder, self->x, self->y + (h - start) * (h / self->height));
      if (end <= h)
        {
          gsk_path_builder_line_to (builder, self->x, self->y + (h - end) * (h / self->height));
          return;
        }
      gsk_path_builder_line_to (builder, self->x, self->y);
    }
}

static int
gsk_rect_contour_get_winding (const GskContour       *contour,
                              gpointer                measure_data,
                              const graphene_point_t *point)
{
  const GskRectContour *self = (const GskRectContour *) contour;
  graphene_rect_t rect;

  graphene_rect_init (&rect, self->x, self->y, self->width, self->height);

  if (graphene_rect_contains_point (&rect, point))
    return -1;

  return 0;
}

static const GskContourClass GSK_RECT_CONTOUR_CLASS =
{
  sizeof (GskRectContour),
  "GskRectContour",
  gsk_rect_contour_copy,
  gsk_contour_get_size_default,
  gsk_rect_contour_get_flags,
  gsk_rect_contour_print,
  gsk_rect_contour_get_bounds,
  gsk_rect_contour_get_start_end,
  gsk_rect_contour_foreach,
  gsk_rect_contour_reverse,
  gsk_rect_contour_init_measure,
  gsk_rect_contour_free_measure,
  gsk_rect_contour_get_point,
  gsk_rect_contour_get_curvature,
  gsk_rect_contour_get_closest_point,
  gsk_rect_contour_add_segment,
  gsk_rect_contour_get_winding,
};

GskContour *
gsk_rect_contour_new (const graphene_rect_t *rect)
{
  GskRectContour *self;

  self = g_new0 (GskRectContour, 1);

  self->contour.klass = &GSK_RECT_CONTOUR_CLASS;

  self->x = rect->origin.x;
  self->y = rect->origin.y;
  self->width = rect->size.width;
  self->height = rect->size.height;

  return (GskContour *) self;
}

/* }}} */
/* {{{ Rounded Rectangle */

typedef struct _GskRoundedRectContour GskRoundedRectContour;
struct _GskRoundedRectContour
{
  GskContour contour;

  GskRoundedRect rect;
  gboolean ccw;
};

static void
gsk_rounded_rect_contour_copy (const GskContour *contour,
                               GskContour       *dest)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;
  GskRoundedRectContour *target = (GskRoundedRectContour *) dest;

  *target = *self;
}

static GskPathFlags
gsk_rounded_rect_contour_get_flags (const GskContour *contour)
{
  return GSK_PATH_CLOSED;
}

static inline void
get_rounded_rect_points (const GskRoundedRect *rect,
                         graphene_point_t     *pts)
{
  pts[0] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->corner[GSK_CORNER_TOP_LEFT].width, rect->bounds.origin.y);
  pts[1] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width - rect->corner[GSK_CORNER_TOP_RIGHT].width, rect->bounds.origin.y);
  pts[2] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width, rect->bounds.origin.y);
  pts[3] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width, rect->bounds.origin.y + rect->corner[GSK_CORNER_TOP_RIGHT].height);
  pts[4] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width, rect->bounds.origin.y + rect->bounds.size.height - rect->corner[GSK_CORNER_BOTTOM_RIGHT].height);
  pts[5] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width, rect->bounds.origin.y + rect->bounds.size.height);
  pts[6] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->bounds.size.width - rect->corner[GSK_CORNER_TOP_RIGHT].width, rect->bounds.origin.y + rect->bounds.size.height);
  pts[7] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->corner[GSK_CORNER_BOTTOM_LEFT].width, rect->bounds.origin.y + rect->bounds.size.height);
  pts[8] = GRAPHENE_POINT_INIT (rect->bounds.origin.x, rect->bounds.origin.y + rect->bounds.size.height);
  pts[9] = GRAPHENE_POINT_INIT (rect->bounds.origin.x, rect->bounds.origin.y + rect->bounds.size.height - rect->corner[GSK_CORNER_BOTTOM_LEFT].height);
  pts[10] = GRAPHENE_POINT_INIT (rect->bounds.origin.x, rect->bounds.origin.y + rect->corner[GSK_CORNER_TOP_LEFT].height);
  pts[11] = GRAPHENE_POINT_INIT (rect->bounds.origin.x, rect->bounds.origin.y);
  pts[12] = GRAPHENE_POINT_INIT (rect->bounds.origin.x + rect->corner[GSK_CORNER_TOP_LEFT].width, rect->bounds.origin.y);
}

static void
gsk_rounded_rect_contour_print (const GskContour *contour,
                                GString          *string)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;
  graphene_point_t pts[13];

  get_rounded_rect_points (&self->rect, pts);

#define APPEND_MOVE(p) \
  g_string_append (string, "M "); \
  _g_string_append_point (string, p);
#define APPEND_LINE(p) \
  g_string_append (string, " L "); \
  _g_string_append_point (string, p);
#define APPEND_CONIC(p1, p2) \
  g_string_append (string, " O "); \
  _g_string_append_point (string, p1); \
  g_string_append (string, ", "); \
  _g_string_append_point (string, p2); \
  g_string_append (string, ", "); \
  _g_string_append_double (string, M_SQRT1_2);
#define APPEND_CLOSE g_string_append (string, " z");
  if (self->ccw)
    {
      graphene_point_t p;
#define SWAP(a,b,c) a = b; b = c; c = a;
      SWAP (p, pts[1], pts[11]);
      SWAP (p, pts[2], pts[10]);
      SWAP (p, pts[3], pts[9]);
      SWAP (p, pts[4], pts[8]);
      SWAP (p, pts[5], pts[7]);
#undef SWAP

      APPEND_MOVE (&pts[0]);
      APPEND_CONIC (&pts[1], &pts[2]);
      APPEND_LINE (&pts[3]);
      APPEND_CONIC (&pts[4], &pts[5]);
      APPEND_LINE (&pts[6]);
      APPEND_CONIC (&pts[7], &pts[8]);
      APPEND_LINE (&pts[9]);
      APPEND_CONIC (&pts[10], &pts[11]);
      APPEND_LINE (&pts[12]);
      APPEND_CLOSE
    }
  else
    {
      APPEND_MOVE (&pts[0]);
      APPEND_LINE (&pts[1]);
      APPEND_CONIC (&pts[2], &pts[3]);
      APPEND_LINE (&pts[4]);
      APPEND_CONIC (&pts[5], &pts[6]);
      APPEND_LINE (&pts[7]);
      APPEND_CONIC (&pts[8], &pts[9]);
      APPEND_LINE (&pts[10]);
      APPEND_CONIC (&pts[11], &pts[12]);
      APPEND_CLOSE;
    }
#undef APPEND_MOVE
#undef APPEND_LINE
#undef APPEND_CONIC
#undef APPEND_CLOSE
}

static gboolean
gsk_rounded_rect_contour_get_bounds (const GskContour *contour,
                                     graphene_rect_t  *rect)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;

  graphene_rect_init_from_rect (rect, &self->rect.bounds);

  return TRUE;
}

static void
gsk_rounded_rect_contour_get_start_end (const GskContour *contour,
                                        graphene_point_t *start,
                                        graphene_point_t *end)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;

  if (start)
    *start = GRAPHENE_POINT_INIT (self->rect.bounds.origin.x + self->rect.corner[GSK_CORNER_TOP_LEFT].width,
                                  self->rect.bounds.origin.y);

  if (end)
    *end = GRAPHENE_POINT_INIT (self->rect.bounds.origin.x + self->rect.corner[GSK_CORNER_TOP_LEFT].width,
                                self->rect.bounds.origin.y);
}

static gboolean
gsk_rounded_rect_contour_foreach (const GskContour   *contour,
                                  float               tolerance,
                                  GskPathForeachFunc  func,
                                  gpointer            user_data)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;
  graphene_point_t pts[13];

  get_rounded_rect_points (&self->rect, pts);
  if (self->ccw)
    {
      graphene_point_t p;
#define SWAP(a,b,c) a = b; b = c; c = a;
      SWAP (p, pts[1], pts[11]);
      SWAP (p, pts[2], pts[10]);
      SWAP (p, pts[3], pts[9]);
      SWAP (p, pts[4], pts[8]);
      SWAP (p, pts[5], pts[7]);
#undef SWAP

      return func (GSK_PATH_MOVE, &pts[0], 1, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[0], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[2], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[3], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[5], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[6], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[8], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[9], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[11], 2, 0, user_data) &&
             func (GSK_PATH_CLOSE, &pts[12], 2, 0, user_data);
    }
  else
    {
      return func (GSK_PATH_MOVE, &pts[0], 1, 0, user_data) &&
             func (GSK_PATH_LINE, &pts[0], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[1], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[3], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[4], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[6], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[7], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_LINE, &pts[9], 2, 0, user_data) &&
             func (GSK_PATH_CONIC, &pts[10], 3, M_SQRT1_2, user_data) &&
             func (GSK_PATH_CLOSE, &pts[12], 2, 0, user_data);
    }
}

static GskContour *
gsk_rounded_rect_contour_reverse (const GskContour *contour)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;
  GskRoundedRectContour *copy;

  copy = g_new0 (GskRoundedRectContour, 1);
  gsk_rounded_rect_contour_copy (contour, (GskContour *)copy);
  copy->ccw = !self->ccw;

  return (GskContour *)copy;
}

typedef struct
{
  GskPath *path;
  const GskContour *contour;
  gpointer measure_data;
} RoundedRectMeasureData;

static gboolean
add_cb (GskPathOperation        op,
        const graphene_point_t *pts,
        gsize                   n_pts,
        float                   weight,
        gpointer                user_data)
{
  GskPathBuilder *builder = user_data;
  switch (op)
    {
    case GSK_PATH_MOVE:
      gsk_path_builder_move_to (builder, pts[0].x, pts[0].y);
      break;
    case GSK_PATH_LINE:
      gsk_path_builder_line_to (builder, pts[1].x, pts[1].y);
      break;
    case GSK_PATH_QUAD:
      gsk_path_builder_quad_to (builder,
                                pts[1].x, pts[1].y,
                                pts[2].x, pts[2].y);
      break;
    case GSK_PATH_CUBIC:
      gsk_path_builder_cubic_to (builder,
                                 pts[1].x, pts[1].y,
                                 pts[2].x, pts[2].y,
                                 pts[3].x, pts[3].y);
      break;
    case GSK_PATH_CONIC:
      gsk_path_builder_conic_to (builder,
                                 pts[1].x, pts[1].y,
                                 pts[3].x, pts[3].y,
                                 weight);
      break;
    case GSK_PATH_CLOSE:
      gsk_path_builder_close (builder);
      break;
    default:
      g_assert_not_reached ();
    }

  return TRUE;
}

static gpointer
gsk_rounded_rect_contour_init_measure (const GskContour *contour,
                                       float             tolerance,
                                       float            *out_length)
{
  GskPathBuilder *builder;
  RoundedRectMeasureData *data;

  builder = gsk_path_builder_new ();

  gsk_contour_foreach (contour, tolerance, add_cb, builder);

  data = g_new (RoundedRectMeasureData, 1);
  data->path = gsk_path_builder_free_to_path (builder);
  data->contour = gsk_path_get_contour (data->path, 0);
  data->measure_data = gsk_standard_contour_init_measure (data->contour, tolerance, out_length);

  return data;
}

static void
gsk_rounded_rect_contour_free_measure (const GskContour *contour,
                                       gpointer          measure_data)
{
  RoundedRectMeasureData *data = measure_data;

  gsk_standard_contour_free_measure (data->contour, data->measure_data);
  gsk_path_unref (data->path);
  g_free (data);
}

static void
gsk_rounded_rect_contour_get_point (const GskContour *contour,
                                    gpointer          measure_data,
                                    float             distance,
                                    GskPathDirection  direction,
                                    graphene_point_t *pos,
                                    graphene_vec2_t  *tangent)
{
  RoundedRectMeasureData *data = measure_data;

  gsk_standard_contour_get_point (data->contour,
                                  data->measure_data,
                                  direction,
                                  distance,
                                  pos,
                                  tangent);
}

static float
gsk_rounded_rect_contour_get_curvature (const GskContour *contour,
                                        gpointer          measure_data,
                                        float             distance,
                                        graphene_point_t *center)
{
  RoundedRectMeasureData *data = measure_data;

  return gsk_standard_contour_get_curvature (data->contour,
                                             data->measure_data,
                                             distance,
                                             center);
}

static gboolean
gsk_rounded_rect_contour_get_closest_point (const GskContour       *contour,
                                            gpointer                measure_data,
                                            float                   tolerance,
                                            const graphene_point_t *point,
                                            float                   threshold,
                                            float                  *out_distance,
                                            graphene_point_t       *out_pos,
                                            float                  *out_offset,
                                            graphene_vec2_t        *out_tangent)
{
  RoundedRectMeasureData *data = measure_data;

  return gsk_standard_contour_get_closest_point (data->contour,
                                                 data->measure_data,
                                                 tolerance,
                                                 point,
                                                 threshold,
                                                 out_distance,
                                                 out_pos,
                                                 out_offset,
                                                 out_tangent);
}

static void
gsk_rounded_rect_contour_add_segment (const GskContour *contour,
                                      GskPathBuilder   *builder,
                                      gpointer          measure_data,
                                      gboolean          emit_move_to,
                                      float             start,
                                      float             end)
{
  RoundedRectMeasureData *data = measure_data;

  gsk_standard_contour_add_segment (data->contour,
                                    builder,
                                    data->measure_data,
                                    emit_move_to,
                                    start,
                                    end);
}

static int
gsk_rounded_rect_contour_get_winding (const GskContour       *contour,
                                      gpointer                measure_data,
                                      const graphene_point_t *point)
{
  const GskRoundedRectContour *self = (const GskRoundedRectContour *) contour;

  if (gsk_rounded_rect_contains_point (&self->rect, point))
    return self->ccw ? 1 : -1;

  return 0;
}

static const GskContourClass GSK_ROUNDED_RECT_CONTOUR_CLASS =
{
  sizeof (GskRoundedRectContour),
  "GskRoundedRectContour",
  gsk_rounded_rect_contour_copy,
  gsk_contour_get_size_default,
  gsk_rounded_rect_contour_get_flags,
  gsk_rounded_rect_contour_print,
  gsk_rounded_rect_contour_get_bounds,
  gsk_rounded_rect_contour_get_start_end,
  gsk_rounded_rect_contour_foreach,
  gsk_rounded_rect_contour_reverse,
  gsk_rounded_rect_contour_init_measure,
  gsk_rounded_rect_contour_free_measure,
  gsk_rounded_rect_contour_get_point,
  gsk_rounded_rect_contour_get_curvature,
  gsk_rounded_rect_contour_get_closest_point,
  gsk_rounded_rect_contour_add_segment,
  gsk_rounded_rect_contour_get_winding,
};

GskContour *
gsk_rounded_rect_contour_new (const GskRoundedRect *rect)
{
  GskRoundedRectContour *self;

  self = g_new0 (GskRoundedRectContour, 1);

  self->contour.klass = &GSK_ROUNDED_RECT_CONTOUR_CLASS;

  self->rect = *rect;

  return (GskContour *) self;
}

/* }}} */
/* {{{ Circle */

#define DEG_TO_RAD(x)          ((x) * (G_PI / 180.f))
#define RAD_TO_DEG(x)          ((x) / (G_PI / 180.f))

typedef struct _GskCircleContour GskCircleContour;
struct _GskCircleContour
{
  GskContour contour;

  graphene_point_t center;
  float radius;
  float start_angle; /* in degrees */
  float end_angle; /* start_angle +/- 360 */
};

static void
gsk_circle_contour_copy (const GskContour *contour,
                         GskContour       *dest)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  GskCircleContour *target = (GskCircleContour *) dest;

  *target = *self;
}

static GskPathFlags
gsk_circle_contour_get_flags (const GskContour *contour)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  /* XXX: should we explicitly close paths? */
  if (fabs (self->start_angle - self->end_angle) >= 360)
    return GSK_PATH_CLOSED;
  else
    return 0;
}

#define GSK_CIRCLE_POINT_INIT(self, angle) \
  GRAPHENE_POINT_INIT ((self)->center.x + cos (DEG_TO_RAD (angle)) * self->radius, \
                       (self)->center.y + sin (DEG_TO_RAD (angle)) * self->radius)

static void
gsk_circle_contour_print (const GskContour *contour,
                          GString          *string)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  float mid_angle = (self->end_angle - self->start_angle) / 2;

  g_string_append (string, "M ");
  _g_string_append_point (string, &GSK_CIRCLE_POINT_INIT (self, self->start_angle));
  g_string_append (string, " A ");
  _g_string_append_point (string, &GRAPHENE_POINT_INIT (self->radius, self->radius));
  g_string_append_printf (string, " 0 0 %u ",
                          self->start_angle < self->end_angle ? 0 : 1);
  _g_string_append_point (string, &GSK_CIRCLE_POINT_INIT (self, mid_angle));
  g_string_append (string, " A ");
  _g_string_append_point (string, &GRAPHENE_POINT_INIT (self->radius, self->radius));
  g_string_append_printf (string, " 0 0 %u ",
                          self->start_angle < self->end_angle ? 0 : 1);
  _g_string_append_point (string, &GSK_CIRCLE_POINT_INIT (self, self->end_angle));
  if (fabs (self->start_angle - self->end_angle) >= 360)
    g_string_append (string, " z");
}

static gboolean
gsk_circle_contour_get_bounds (const GskContour *contour,
                               graphene_rect_t  *rect)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  /* XXX: handle partial circles */
  graphene_rect_init (rect,
                      self->center.x - self->radius,
                      self->center.y - self->radius,
                      2 * self->radius,
                      2 * self->radius);

  return TRUE;
}

static void
gsk_circle_contour_get_start_end (const GskContour *contour,
                                  graphene_point_t *start,
                                  graphene_point_t *end)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  if (start)
    *start = GSK_CIRCLE_POINT_INIT (self, self->start_angle);

  if (end)
    *end = GSK_CIRCLE_POINT_INIT (self, self->end_angle);
}

typedef struct
{
  GskPathForeachFunc func;
  gpointer           user_data;
} ForeachWrapper;

static gboolean
gsk_circle_contour_curve (const graphene_point_t curve[4],
                          gpointer               data)
{
  ForeachWrapper *wrapper = data;

  return wrapper->func (GSK_PATH_CUBIC, curve, 4, 0, wrapper->user_data);
}

static gboolean
gsk_circle_contour_foreach (const GskContour   *contour,
                            float               tolerance,
                            GskPathForeachFunc  func,
                            gpointer            user_data)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  graphene_point_t start = GSK_CIRCLE_POINT_INIT (self, self->start_angle);

  if (!func (GSK_PATH_MOVE, &start, 1, 0, user_data))
    return FALSE;

  if (!gsk_spline_decompose_arc (&self->center,
                                 self->radius,
                                 tolerance,
                                 DEG_TO_RAD (self->start_angle),
                                 DEG_TO_RAD (self->end_angle),
                                 gsk_circle_contour_curve,
                                 &(ForeachWrapper) { func, user_data }))
    return FALSE;

  if (fabs (self->start_angle - self->end_angle) >= 360)
    {
      if (!func (GSK_PATH_CLOSE, (graphene_point_t[2]) { start, start }, 2, 0, user_data))
        return FALSE;
    }

  return TRUE;
}

static GskContour *
gsk_circle_contour_reverse (const GskContour *contour)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  return gsk_circle_contour_new (&self->center,
                                 self->radius,
                                 self->end_angle,
                                 self->start_angle);
}

static gpointer
gsk_circle_contour_init_measure (const GskContour *contour,
                                 float             tolerance,
                                 float            *out_length)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  *out_length = DEG_TO_RAD (fabs (self->start_angle - self->end_angle)) * self->radius;

  return NULL;
}

static void
gsk_circle_contour_free_measure (const GskContour *contour,
                                 gpointer          data)
{
}

static void
gsk_circle_contour_get_point (const GskContour *contour,
                              gpointer          measure_data,
                              float             distance,
                              GskPathDirection  direction,
                              graphene_point_t *pos,
                              graphene_vec2_t  *tangent)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  float delta = self->end_angle - self->start_angle;
  float length = self->radius * DEG_TO_RAD (delta);
  float angle = self->start_angle + distance/length * delta;
  graphene_point_t p;

  p = GSK_CIRCLE_POINT_INIT (self, angle);

  if (pos)
    *pos = p;

  if (tangent)
    {
      graphene_vec2_init (tangent,
                          p.y - self->center.y,
                          - p.x + self->center.x);
      graphene_vec2_normalize (tangent, tangent);
    }
}

static float
gsk_circle_contour_get_curvature (const GskContour *contour,
                                  gpointer          measure_data,
                                  float             distance,
                                  graphene_point_t *center)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  if (center)
    *center = self->center;

  return 1 / self->radius;
}

static gboolean
gsk_circle_contour_get_closest_point (const GskContour       *contour,
                                      gpointer                measure_data,
                                      float                   tolerance,
                                      const graphene_point_t *point,
                                      float                   threshold,
                                      float                  *out_distance,
                                      graphene_point_t       *out_pos,
                                      float                  *out_offset,
                                      graphene_vec2_t        *out_tangent)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  float angle;
  float closest_angle;
  float offset;
  graphene_point_t pos;
  graphene_vec2_t tangent;
  float distance;

  if (graphene_point_distance (point, &self->center, NULL, NULL) > threshold + self->radius)
    return FALSE;

  angle = atan2f (point->y - self->center.y, point->x - self->center.x);
  angle = RAD_TO_DEG (angle);
  if (angle < 0)
    angle += 360;

  if ((self->start_angle <= angle && angle <= self->end_angle) ||
      (self->end_angle <= angle && angle <= self->start_angle))
    {
      closest_angle = angle;
    }
  else
    {
      float d1, d2;

      d1 = fabs (self->start_angle - angle);
      d1 = MIN (d1, 360 - d1);
      d2 = fabs (self->end_angle - angle);
      d2 = MIN (d2, 360 - d2);
      if (d1 < d2)
        closest_angle = self->start_angle;
      else
        closest_angle = self->end_angle;
    }

  offset = self->radius * 2 * M_PI * (closest_angle - self->start_angle) / (self->end_angle - self->start_angle);

  gsk_circle_contour_get_point (contour, NULL, offset, GSK_PATH_END, &pos, out_tangent ? &tangent : NULL);

  distance = graphene_point_distance (&pos, point, NULL, NULL);
  if (threshold < distance)
    return FALSE;

  if (out_offset)
    *out_offset = offset;
  if (out_pos)
    *out_pos = pos;
  if (out_distance)
    *out_distance = distance;
  if (out_tangent)
    *out_tangent = tangent;

  return TRUE;
}

static gboolean
add_curve_to_segment (GskPathOperation        op,
                      const graphene_point_t *pts,
                      gsize                   n_pts,
                      float                   weight,
                      gpointer                data)
{
  GskPathBuilder *builder = data;
  GskCurve curve;

  gsk_curve_init_foreach (&curve, op, pts, n_pts, weight);
  gsk_curve_builder_to (&curve, builder);

  return TRUE;
}

static void
gsk_circle_contour_add_segment (const GskContour *contour,
                                GskPathBuilder   *builder,
                                gpointer          measure_data,
                                gboolean          emit_move_to,
                                float             start,
                                float             end)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;
  float delta = self->end_angle - self->start_angle;
  float length = self->radius * DEG_TO_RAD (delta);
  float start_angle = self->start_angle + start/length * delta;
  float end_angle = self->start_angle + end/length * delta;

  if (emit_move_to)
    {
      GskContour *segment;

      segment = gsk_circle_contour_new (&self->center, self->radius,
                                        start_angle, end_angle);
      gsk_path_builder_add_contour (builder, segment);
    }
  else
    {
      /* convert to a standard contour */
      gsk_spline_decompose_arc (&self->center,
                                self->radius,
                                GSK_PATH_TOLERANCE_DEFAULT,
                                DEG_TO_RAD (start_angle),
                                DEG_TO_RAD (end_angle),
                                gsk_circle_contour_curve,
                                &(ForeachWrapper) { add_curve_to_segment, builder });
    }
}

static int
gsk_circle_contour_get_winding (const GskContour       *contour,
                                gpointer                measure_data,
                                const graphene_point_t *point)
{
  const GskCircleContour *self = (const GskCircleContour *) contour;

  if (graphene_point_distance (point, &self->center, NULL, NULL) >= self->radius)
    return 0;

  if (fabs (self->start_angle - self->end_angle) >= 360)
    {
      return -1;
    }
  else
    {
      /* Check if the point and the midpoint are on the same side
       * of the chord through start and end.
       */
      double mid_angle = (self->end_angle - self->start_angle) / 2;
      graphene_point_t start = GRAPHENE_POINT_INIT (self->center.x + cos (DEG_TO_RAD (self->start_angle)) * self->radius,
                                                    self->center.y + sin (DEG_TO_RAD (self->start_angle)) * self->radius);
      graphene_point_t mid = GRAPHENE_POINT_INIT (self->center.x + cos (DEG_TO_RAD (mid_angle)) * self->radius,
                                                  self->center.y + sin (DEG_TO_RAD (mid_angle)) * self->radius);
      graphene_point_t end = GRAPHENE_POINT_INIT (self->center.x + cos (DEG_TO_RAD (self->end_angle)) * self->radius,
                                                  self->center.y + sin (DEG_TO_RAD (self->end_angle)) * self->radius);

      graphene_vec2_t n, m;
      float a, b;

      graphene_vec2_init (&n, start.y - end.y, end.x - start.x);
      graphene_vec2_init (&m, mid.x, mid.y);
      a = graphene_vec2_dot (&m, &n);
      graphene_vec2_init (&m, point->x, point->y);
      b = graphene_vec2_dot (&m, &n);

      if ((a < 0) != (b < 0))
        return -1;
    }

  return 0;
}

static const GskContourClass GSK_CIRCLE_CONTOUR_CLASS =
{
  sizeof (GskCircleContour),
  "GskCircleContour",
  gsk_circle_contour_copy,
  gsk_contour_get_size_default,
  gsk_circle_contour_get_flags,
  gsk_circle_contour_print,
  gsk_circle_contour_get_bounds,
  gsk_circle_contour_get_start_end,
  gsk_circle_contour_foreach,
  gsk_circle_contour_reverse,
  gsk_circle_contour_init_measure,
  gsk_circle_contour_free_measure,
  gsk_circle_contour_get_point,
  gsk_circle_contour_get_curvature,
  gsk_circle_contour_get_closest_point,
  gsk_circle_contour_add_segment,
  gsk_circle_contour_get_winding,
};

GskContour *
gsk_circle_contour_new (const graphene_point_t *center,
                        float                   radius,
                        float                   start_angle,
                        float                   end_angle)
{
  GskCircleContour *self;

  self = g_new0 (GskCircleContour, 1);

  self->contour.klass = &GSK_CIRCLE_CONTOUR_CLASS;

  g_assert (fabs (start_angle - end_angle) <= 360);

  self->contour.klass = &GSK_CIRCLE_CONTOUR_CLASS;
  self->center = *center;
  self->radius = radius;
  self->start_angle = start_angle;
  self->end_angle = end_angle;

  return (GskContour *) self;
}

/* }}} */
/* {{{ API */

gsize
gsk_contour_get_size (const GskContour *self)
{
  return self->klass->get_size (self);
}

void
gsk_contour_copy (GskContour       *dest,
                  const GskContour *src)
{
  src->klass->copy (src, dest);
}

GskContour *
gsk_contour_dup (const GskContour *src)
{
  GskContour *copy;

  copy = g_malloc0 (gsk_contour_get_size (src));
  gsk_contour_copy (copy, src);

  return copy;
}

GskContour *
gsk_contour_reverse (const GskContour *src)
{
  return src->klass->reverse (src);
}

GskPathFlags
gsk_contour_get_flags (const GskContour *self)
{
  return self->klass->get_flags (self);
}

void
gsk_contour_print (const GskContour *self,
                   GString          *string)
{
  self->klass->print (self, string);
}

gboolean
gsk_contour_get_bounds (const GskContour *self,
                        graphene_rect_t  *bounds)
{
  return self->klass->get_bounds (self, bounds);
}

gboolean
gsk_contour_foreach (const GskContour   *self,
                     float               tolerance,
                     GskPathForeachFunc  func,
                     gpointer            user_data)
{
  return self->klass->foreach (self, tolerance, func, user_data);
}

gpointer
gsk_contour_init_measure (const GskContour *self,
                          float             tolerance,
                          float            *out_length)
{
  return self->klass->init_measure (self, tolerance, out_length);
}

void
gsk_contour_free_measure (const GskContour *self,
                          gpointer          data)
{
  self->klass->free_measure (self, data);
}

void
gsk_contour_get_start_end (const GskContour *self,
                           graphene_point_t *start,
                           graphene_point_t *end)
{
  self->klass->get_start_end (self, start, end);
}

void
gsk_contour_get_point (const GskContour *self,
                       gpointer          measure_data,
                       float             distance,
                       GskPathDirection  direction,
                       graphene_point_t *pos,
                       graphene_vec2_t  *tangent)
{
  self->klass->get_point (self, measure_data, distance, direction, pos, tangent);
}

gboolean
gsk_contour_get_closest_point (const GskContour       *self,
                               gpointer                measure_data,
                               float                   tolerance,
                               const graphene_point_t *point,
                               float                   threshold,
                               float                  *out_distance,
                               graphene_point_t       *out_pos,
                               float                  *out_offset,
                               graphene_vec2_t        *out_tangent)
{
  return self->klass->get_closest_point (self,
                                         measure_data,
                                         tolerance,
                                         point,
                                         threshold,
                                         out_distance,
                                         out_pos,
                                         out_offset,
                                         out_tangent);
}

void
gsk_contour_add_segment (const GskContour *self,
                         GskPathBuilder   *builder,
                         gpointer          measure_data,
                         gboolean          emit_move_to,
                         float             start,
                         float             end)
{
  self->klass->add_segment (self, builder, measure_data, emit_move_to, start, end);
}

int
gsk_contour_get_winding (const GskContour       *self,
                         gpointer                measure_data,
                         const graphene_point_t *point)
{
  return self->klass->get_winding (self, measure_data, point);
}

float
gsk_contour_get_curvature (const GskContour *self,
                           gpointer          measure_data,
                           float             distance,
                           graphene_point_t *center)
{
  return self->klass->get_curvature (self, measure_data, distance, center);
}

/* }}} */

/* vim:set foldmethod=marker expandtab: */
