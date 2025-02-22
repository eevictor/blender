/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

#pragma once

/** \file
 * \ingroup bke
 *
 * Basic design of the DerivedMesh system:
 *
 * DerivedMesh is a common set of interfaces for mesh systems.
 *
 * There are three main mesh data structures in Blender:
 * #Mesh, #CDDerivedMesh and #BMesh.
 *
 * These, and a few others, all implement DerivedMesh interfaces,
 * which contains unified drawing interfaces, a few utility interfaces,
 * and a bunch of read-only interfaces intended mostly for conversion from
 * one format to another.
 *
 * All Mesh structures in blender make use of CustomData, which is used to store
 * per-element attributes and interpolate them (e.g. uvs, vcols, vgroups, etc).
 *
 * Mesh is the "serialized" structure, used for storing object-mode mesh data
 * and also for saving stuff to disk.  Its interfaces are also what DerivedMesh
 * uses to communicate with.
 *
 * CDDM is a little mesh library, that uses Mesh data structures in the backend.
 * It's mostly used for modifiers, and has the advantages of not taking much
 * resources.
 *
 * BMesh is a full-on brep, used for editmode, some modifiers, etc.  It's much
 * more capable (if memory-intensive) then CDDM.
 *
 * DerivedMesh is somewhat hackish.  Many places assumes that a DerivedMesh is
 * a CDDM (most of the time by simply copying it and converting it to one).
 * CDDM is the original structure for modifiers, but has since been superseded
 * by BMesh, at least for the foreseeable future.
 */

/*
 * NOTE: This structure is read-only, for all practical purposes.
 *       At some point in the future, we may want to consider
 *       creating a replacement structure that implements a proper
 *       abstract mesh kernel interface.  Or, we can leave this
 *       as it is and stick with using BMesh and CDDM.
 */

#include "BLI_compiler_attrs.h"

#include "DNA_customdata_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BMEditMesh;
struct CCGElem;
struct CCGKey;
struct CustomData_MeshMasks;
struct Depsgraph;
struct MEdge;
struct MFace;
struct MVert;
struct Mesh;
struct ModifierData;
struct Object;
struct Scene;

/*
 * NOTE: all #MFace interfaces now officially operate on tessellated data.
 *       Also, the #MFace orig-index layer indexes #MPoly, not #MFace.
 */

/* keep in sync with MFace/MPoly types */
typedef struct DMFlagMat {
  short mat_nr;
  char flag;
} DMFlagMat;

typedef enum DerivedMeshType {
  DM_TYPE_CDDM,
  DM_TYPE_CCGDM,
} DerivedMeshType;

typedef struct DerivedMesh DerivedMesh;
struct DerivedMesh {
  /** Private DerivedMesh data, only for internal DerivedMesh use */
  CustomData vertData, edgeData, faceData, loopData, polyData;
  int numVertData, numEdgeData, numTessFaceData, numLoopData, numPolyData;
  int needsFree;    /* checked on ->release, is set to 0 for cached results */
  int deformedOnly; /* set by modifier stack if only deformed from original */
  DerivedMeshType type;

  /**
   * \warning Typical access is done via #getLoopTriArray, #getNumLoopTri.
   */
  struct {
    /* WARNING! swapping between array (ready-to-be-used data) and array_wip
     * (where data is actually computed) shall always be protected by same
     * lock as one used for looptris computing. */
    struct MLoopTri *array, *array_wip;
    int num;
    int num_alloc;
  } looptris;

  /* use for converting to BMesh which doesn't store bevel weight and edge crease by default */
  char cd_flag;

  short tangent_mask; /* which tangent layers are calculated */

  /** Loop tessellation cache (WARNING! Only call inside threading-protected code!) */
  void (*recalcLoopTri)(DerivedMesh *dm);
  /** accessor functions */
  const struct MLoopTri *(*getLoopTriArray)(DerivedMesh *dm);
  int (*getNumLoopTri)(DerivedMesh *dm);

  /* Misc. Queries */

  /* Also called in Editmode */
  int (*getNumVerts)(DerivedMesh *dm);
  int (*getNumEdges)(DerivedMesh *dm);
  int (*getNumLoops)(DerivedMesh *dm);
  int (*getNumPolys)(DerivedMesh *dm);

  /** Return a pointer to the entire array of verts/edges/face from the
   * derived mesh. if such an array does not exist yet, it will be created,
   * and freed on the next ->release(). consider using getVert/Edge/Face if
   * you are only interested in a few verts/edges/faces.
   */
  struct MVert *(*getVertArray)(DerivedMesh *dm);
  struct MEdge *(*getEdgeArray)(DerivedMesh *dm);
  struct MLoop *(*getLoopArray)(DerivedMesh *dm);
  struct MPoly *(*getPolyArray)(DerivedMesh *dm);

  /** Copy all verts/edges/faces from the derived mesh into
   * *{vert/edge/face}_r (must point to a buffer large enough)
   */
  void (*copyVertArray)(DerivedMesh *dm, struct MVert *r_vert);
  void (*copyEdgeArray)(DerivedMesh *dm, struct MEdge *r_edge);
  void (*copyLoopArray)(DerivedMesh *dm, struct MLoop *r_loop);
  void (*copyPolyArray)(DerivedMesh *dm, struct MPoly *r_poly);

  /** Return a pointer to the entire array of vert/edge/face custom data
   * from the derived mesh (this gives a pointer to the actual data, not
   * a copy)
   */
  void *(*getVertDataArray)(DerivedMesh *dm, int type);
  void *(*getEdgeDataArray)(DerivedMesh *dm, int type);
  void *(*getLoopDataArray)(DerivedMesh *dm, int type);
  void *(*getPolyDataArray)(DerivedMesh *dm, int type);

  /** Optional grid access for subsurf */
  int (*getNumGrids)(DerivedMesh *dm);
  int (*getGridSize)(DerivedMesh *dm);
  struct CCGElem **(*getGridData)(DerivedMesh *dm);
  int *(*getGridOffset)(DerivedMesh *dm);
  void (*getGridKey)(DerivedMesh *dm, struct CCGKey *key);
  DMFlagMat *(*getGridFlagMats)(DerivedMesh *dm);
  unsigned int **(*getGridHidden)(DerivedMesh *dm);

  /** Direct Access Operations
   * - Can be undefined
   * - Must be defined for modifiers that only deform however */

  /** Get vertex location, undefined if index is not valid */
  void (*getVertCo)(DerivedMesh *dm, int index, float r_co[3]);

  /** Get smooth vertex normal, undefined if index is not valid */
  void (*getVertNo)(DerivedMesh *dm, int index, float r_no[3]);

  /** Release reference to the DerivedMesh. This function decides internally
   * if the DerivedMesh will be freed, or cached for later use. */
  void (*release)(DerivedMesh *dm);
};

/**
 * Utility function to initialize a #DerivedMesh's function pointers to
 * the default implementation (for those functions which have a default).
 */
void DM_init_funcs(DerivedMesh *dm);

/**
 * Utility function to initialize a #DerivedMesh for the desired number
 * of vertices, edges and faces (doesn't allocate memory for them, just
 * sets up the custom data layers)>
 */
void DM_init(DerivedMesh *dm,
             DerivedMeshType type,
             int numVerts,
             int numEdges,
             int numTessFaces,
             int numLoops,
             int numPolys);

/**
 * Utility function to initialize a DerivedMesh for the desired number
 * of vertices, edges and faces, with a layer setup copied from source
 */
void DM_from_template(DerivedMesh *dm,
                      DerivedMesh *source,
                      DerivedMeshType type,
                      int numVerts,
                      int numEdges,
                      int numTessFaces,
                      int numLoops,
                      int numPolys);

/**
 * Utility function to release a DerivedMesh's layers
 * returns true if DerivedMesh has to be released by the backend, false otherwise.
 */
bool DM_release(DerivedMesh *dm);

/**
 * set the #CD_FLAG_NOCOPY flag in custom data layers where the mask is
 * zero for the layer type, so only layer types specified by the mask
 * will be copied
 */
void DM_set_only_copy(DerivedMesh *dm, const struct CustomData_MeshMasks *mask);

/* -------------------------------------------------------------------- */
/** \name Custom Data Layer Access Functions
 *
 * \return pointer to first data layer which matches type (a flat array)
 * if they return NULL, data doesn't exist.
 * \note these return pointers - any change modifies the internals of the mesh.
 * \{ */

void *DM_get_vert_data_layer(struct DerivedMesh *dm, int type);
void *DM_get_edge_data_layer(struct DerivedMesh *dm, int type);
void *DM_get_poly_data_layer(struct DerivedMesh *dm, int type);
void *DM_get_loop_data_layer(struct DerivedMesh *dm, int type);

/** \} */

/**
 * Custom data copy functions
 * copy count elements from source_index in source to dest_index in dest
 * these copy all layers for which the CD_FLAG_NOCOPY flag is not set.
 */
void DM_copy_vert_data(struct DerivedMesh *source,
                       struct DerivedMesh *dest,
                       int source_index,
                       int dest_index,
                       int count);

/**
 * Ensure the array is large enough.
 *
 * \note This function must always be thread-protected by caller.
 * It should only be used by internal code.
 */
void DM_ensure_looptri_data(DerivedMesh *dm);

/**
 * Interpolates vertex data from the vertices indexed by `src_indices` in the
 * source mesh using the given weights and stores the result in the vertex
 * indexed by `dest_index` in the `dest` mesh.
 */
void DM_interp_vert_data(struct DerivedMesh *source,
                         struct DerivedMesh *dest,
                         int *src_indices,
                         float *weights,
                         int count,
                         int dest_index);

void mesh_get_mapped_verts_coords(struct Mesh *me_eval, float (*r_cos)[3], int totcos);

/**
 * Same as above but won't use render settings.
 */
struct Mesh *editbmesh_get_eval_cage(struct Depsgraph *depsgraph,
                                     const struct Scene *scene,
                                     struct Object *obedit,
                                     struct BMEditMesh *em,
                                     const struct CustomData_MeshMasks *dataMask);
struct Mesh *editbmesh_get_eval_cage_from_orig(struct Depsgraph *depsgraph,
                                               const struct Scene *scene,
                                               struct Object *obedit,
                                               const struct CustomData_MeshMasks *dataMask);

float (*editbmesh_vert_coords_alloc(struct BMEditMesh *em, int *r_vert_len))[3];
bool editbmesh_modifier_is_enabled(const struct Scene *scene,
                                   const struct Object *ob,
                                   struct ModifierData *md,
                                   bool has_prev_mesh);
void makeDerivedMesh(struct Depsgraph *depsgraph,
                     const struct Scene *scene,
                     struct Object *ob,
                     const struct CustomData_MeshMasks *dataMask);

#ifdef __cplusplus
}
#endif
