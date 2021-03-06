#include "sdf.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include "matrix.h"
#include "canon_sdf.h"
#include <glm/gtx/string_cast.hpp>
#include <atomic>
#include <thread>
#include <chrono>

ctpl::thread_pool sdf_t::pool(8);
std::vector<std::future<void>> sdf_t::futures;
bool sdf_t::is_initialised = false;
sdf_t::deform_field_t sdf_t::deform_field;

void
sdf_t::pool_wait(){
    for (int i = 0; i < futures.size(); i++){
        futures[i].get();
    }
    futures.clear();
}

sdf_t::sdf_t(depth_map_t depths, min_params_t * ps){
    this->depths = depths;
    this->ps = ps;   

    float l = ps->voxel_length;
    point_t size = ps->size;

    if (!is_initialised){
        std::cout << "Initialising deformation field..." << std::endl; 

        deform_field = std::vector<std::vector<std::vector<point_t>>>(
            size.x / l,
            std::vector<std::vector<point_t>>(
                size.y / l,
                std::vector<point_t>(
                    size.z / l, 
                    point_t()
                )
            )
        );
 
        is_initialised = true;
    }

    phi = new function_t<float>([&](point_t p){
        return distance(p);
    });

    psi = new function_t<point_t>([&](point_t p){
        return deformation_at(p);
    });

    psi_u = new function_t<float>([&](point_t p){
        return deformation_at(p).x;
    });
    
    psi_v = new function_t<float>([&](point_t p){
        return deformation_at(p).y;
    });
    
    psi_w = new function_t<float>([&](point_t p){
        return deformation_at(p).z;
    });
}

sdf_t::~sdf_t(){
    delete depths;
    delete phi;
    delete psi;
    delete psi_u;
    delete psi_v;
    delete psi_w;
}

float
sdf_t::interpolate1D(float a, float b, float alpha){
    return a * (1 - alpha) + b * alpha;
}

float
sdf_t::interpolate2D(float a, float b, float c, float d, float alpha, float beta){
    float s = interpolate1D(a, b, alpha);
    float t = interpolate1D(c, d, alpha);
    return interpolate1D(s, t, beta);
}

float
sdf_t::interpolate3D(float * xs, float alpha, float beta, float gamma){
    float s = interpolate2D(xs[0], xs[1], xs[2], xs[3], alpha, beta);
    float t = interpolate2D(xs[4], xs[5], xs[6], xs[7], alpha, beta);
    return interpolate1D(s, t, gamma);
}

float
sdf_t::phi_true(point_t v){
    auto project_depth = [&](point_t p){	
        int x = p.x + ps->voxel_length / 2.0f;
        int y = p.y + ps->voxel_length / 2.0f;

        if (x < 0 || y < 0 || x >= depths->size() || y >= depths->at(0).size()){
	    return 0.0f;
        }

        int map = depths->at(x).at(y);
        if (map == 0){
	    return 0.0f;
        }
        return map - p.z;
    };

    point_t v_def = v + deformation_at(v + point_t(ps->voxel_length / 2.0f));
    point_t p_grid = v_def / ps->voxel_length;
    p_grid = point_t((int) p_grid.x, (int) p_grid.y, (int) p_grid.z);
    point_t alpha = (v_def / ps->voxel_length) - p_grid; 
    
    float values[8];
    for (int i = 0; i < 8; i++){
        point_t c = p_grid;
        if (i & 1) c += point_t(1, 0, 0);
        if (i & 2) c += point_t(0, 1, 0);
        if (i & 4) c += point_t(0, 0, 1);
        values[i] = project_depth(c * ps->voxel_length); 
    }

    float result = interpolate3D(values, alpha.x, alpha.y, alpha.z);  
    return result;
}

float
sdf_t::distance(point_t p){
    float d = phi_true(p) / ps->delta;
    
    if (d > 1.0f) return 1.0f;
    if (d < -1.0f) return -1.0f;
    
    return d;
}

point_t
sdf_t::deformation_at(point_t p){
    // align to grid
    point_t v = p / ps->voxel_length;

    int x = v.x;
    int y = v.y;
    int z = v.z;

    if ( 
        x < 0 || y < 0 || z < 0 ||
        x >= deform_field.size() ||
        y >= deform_field[x].size() ||
        z >= deform_field[x][y].size()
    ){
        return point_t();
    }

    return deform_field[x][y][z];
}

point_t
sdf_t::distance_gradient(point_t p){
    return point_t(
        phi->differentiate(0)(p),
        phi->differentiate(1)(p),
        phi->differentiate(2)(p)
    );
}

void 
sdf_t::fuse(canon_sdf_t * canon){
    // rigid component
    bool should_update = true;
    for (int i = 1; should_update; i++){
	std::cout << "Rigid transformation, iteration " << i << "..." << std::endl;

        should_update = false;
        update(true, &should_update, canon);
    }
    std::cout << "Rigid transformation converged." << std::endl;

    // non-rigid component
    should_update = true;
    for (int i = 1; should_update; i++){
        std::cout << "Non-rigid transformation, iteration " << i << "..." << std::endl;

        should_update = false;
        update(false, &should_update, canon);
    }
    std::cout << "Non-rigid transformation converged." << std::endl;
}

void
sdf_t::update(bool is_rigid, bool * cont, canon_sdf_t * canon){
    std::atomic<int> n;
    n.store(0);

    // anonymous function to be evaluated at each voxel
    auto f = [&](int id, int x, int y, int z){
        point_t p = point_t(x, y, z) * ps->voxel_length;

	// calculate energy gradient appropriate to current mode
        point_t e = is_rigid ? 
            data_energy(p, canon) :
            energy(p, canon, ps->omega_k, ps->omega_s, ps->gamma, ps->epsilon);

        float eta = is_rigid ? ps->eta_rigid : ps->eta_nonrigid;

	// apply gradient descent algorithm, set continue flag if update large enough
        if (glm::length(e) > ps->threshold) {
            *cont = true;
            n++;
        }
        deform_field[x][y][z] -= e * eta;

	// perform check on deformation field to see if it has diverged
	point_t d = deform_field[x][y][z];
        if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)){
            std::cout << "Error: deformation field has diverged: " 
		      << glm::to_string(deform_field[x][y][z])
		      << " at: " << glm::to_string(p) << std::endl;
            throw -1;
        }
    };

    // iterate over full volume, in serial or parallel as appropriate
    for (int x = 0; x < deform_field.size(); x++){
        for (int y = 0; y < deform_field[0].size(); y++){
            for (int z = 0; z < deform_field[0][0].size(); z++){
                if (ps->is_multithreaded){
                    futures.push_back(pool.push(f, x, y, z));        
                } else {
	            f(0, x, y, z);
                }
            }
	}
    }
     
    // wait for all threads to finish
    pool_wait();

    if (n > 0){
        std::cout << n << " voxels failed to converge." << std::endl; 
    }
}

point_t
sdf_t::energy(point_t v, canon_sdf_t * c, float o_k, float o_s, float gamma, float eps){
    // function that calculates the three components of the energy gradient as 
    // outlined in the killing fusion paper 
    return 
         data_energy(v, c) +
         killing_energy(v, gamma) * o_k +
         level_set_energy(v, eps) * o_s;
}

point_t
sdf_t::data_energy(point_t p, canon_sdf_t * canon){
    // rigid component of energy gradient
    auto r = distance_gradient(p) * (distance(p) - canon->distance(p));
    return r;
}

point_t
sdf_t::level_set_energy(point_t p, float epsilon){
    // level set energy gradient component
    // requires the magnitude of the gradient of the SDF to be unity
    matrix_t h  = matrix_t::hessian(*phi, p);
    point_t g   = distance_gradient(p);
    float alpha = (glm::length(g) - 1) / (glm::length(g) + epsilon);

    return h * g * alpha;
}

point_t
sdf_t::killing_energy(point_t p, float gamma){
    // killing field energy gradient component
    // requires the deformation field to be a killing vector field
    matrix_t j = matrix_t::jacobian(*psi, p);
    std::vector<float> j_v = j.stack();
    std::vector<float> jt_v = j.transpose().stack();

    std::vector<float> v;
    for (int i = 0; i < j_v.size(); i++){
	v.push_back(jt_v[i] + j_v[i] * gamma);
    }

    matrix_t h_u = matrix_t::hessian(*psi_u, p);
    matrix_t h_v = matrix_t::hessian(*psi_v, p);
    matrix_t h_w = matrix_t::hessian(*psi_w, p);
    matrix_t h[3] = { h_u, h_v, h_w };    
    
    point_t result;
    for (int i = 0; i < 9; i++){
	result += point_t(
            h[i / 3].get(i % 3, 0) * v[i],
            h[i / 3].get(i % 3, 1) * v[i],
            h[i / 3].get(i % 3, 2) * v[i]
	);
    }

    return result * 2.0f;
}
