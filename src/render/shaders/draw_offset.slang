// MeshDraw::drawOffset, shared by the mesh and wire vertex shaders.

/**
 * p moved `offset` cm toward the camera along its view ray, which leaves where it lands on screen alone. vp is the
 * view-projection (reversed-Z), eye the camera position in the same space.
 */
float3 towardCamera(float3 p, float4x4 vp, float3 eye, float offset)
{
    if (offset <= 0.0)
        return p;
    if (all(vp[3].xyz == 0.0))
    {
        // Orthographic: the view direction is the one along which x and y stay put; depth grows toward the camera.
        float3 d = normalize(cross(vp[0].xyz, vp[1].xyz));
        return p + (dot(vp[2].xyz, d) > 0.0 ? d : -d) * offset;
    }
    float3 toEye = eye - p;
    float dist = length(toEye);
    return dist > 0.0 ? p + toEye * (min(offset, 0.5 * dist) / dist) : p;
}
