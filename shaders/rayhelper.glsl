void gsnGetIntersectionBox(int g_InstanceID, int g_PrimitiveID, out vec3 minBB, out vec3 maxBB) {
	minBB = allAabbs[g_InstanceID].i[g_PrimitiveID].minimum;
	maxBB = allAabbs[g_InstanceID].i[g_PrimitiveID].maximum;
}

void gsnGetNormals(int g_InstanceID, int g_PrimitiveID, out vec3 n0, out vec3 n1, out vec3 n2) {
  // Object of this instance
  uint objId = scnDesc.i[g_InstanceID].objId;

  // Indices of the triangle
  ivec3 ind = ivec3(indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 0],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 1],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 2]);
  // Vertex of the triangle
  Vertex v0 = vertices[nonuniformEXT(objId)].v[ind.x];
  Vertex v1 = vertices[nonuniformEXT(objId)].v[ind.y];
  Vertex v2 = vertices[nonuniformEXT(objId)].v[ind.z];

  n0 = v0.nrm;
  n1 = v1.nrm;
  n2 = v2.nrm;
}

void gsnGetTexCoords(int g_InstanceID, int g_PrimitiveID, out vec2 t0, out vec2 t1, out vec2 t2) {
  // Object of this instance
  uint objId = scnDesc.i[g_InstanceID].objId;

  // Indices of the triangle
  ivec3 ind = ivec3(indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 0],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 1],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 2]);
  // Vertex of the triangle
  Vertex v0 = vertices[nonuniformEXT(objId)].v[ind.x];
  Vertex v1 = vertices[nonuniformEXT(objId)].v[ind.y];
  Vertex v2 = vertices[nonuniformEXT(objId)].v[ind.z];

  t0 = v0.texCoord;
  t1 = v1.texCoord;
  t2 = v2.texCoord;
}

void gsnGetPositions(int g_InstanceID, int g_PrimitiveID, out vec3 p0, out vec3 p1, out vec3 p2) {
  // Object of this instance
  uint objId = scnDesc.i[g_InstanceID].objId;

  // Indices of the triangle
  ivec3 ind = ivec3(indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 0],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 1],
                    indices[nonuniformEXT(objId)].i[3 * g_PrimitiveID + 2]);
  // Vertex of the triangle
  Vertex v0 = vertices[nonuniformEXT(objId)].v[ind.x];
  Vertex v1 = vertices[nonuniformEXT(objId)].v[ind.y];
  Vertex v2 = vertices[nonuniformEXT(objId)].v[ind.z];

  p0 = v0.pos;
  p1 = v1.pos;
  p2 = v2.pos;
}

void gsnGetNormal3x3Matrix(int g_InstanceID, out mat3 normalMat) {
	normalMat = mat3(scnDesc.i[g_InstanceID].transfoIT);
}