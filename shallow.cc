/*
 * Jiang-Tadmor centered difference scheme with MinMod limiter,
 * applied to the shallow water equations (St. Venant).
 *
 * See Moler's "Experiments in MATLAB"
 * [moler]: https://www.mathworks.com/moler/exm/chapters/water.pdf
 */

#include <cstdio>
#include <cmath>
#include <cassert>

#include <vector>
#include <algorithm>
#include <array>

using namespace std;


/********************************************************************
 * # Define shallow water physics
 *
 * The shallow water equations are a system of conservation equations
 * relating the depth, horizontal momentum, and vertical momentum for
 * each water column in a domain.  We write these equations in the form
 * \[
 *   U_t = F(U)_x + G(U)_y
 * \]
 * where
 * \[
 *   U = \begin{bmatrix} h \\ hu \\ hv \end{bmatrix},
 *   F = \begin{bmatrix} hu \\ h^2 u + gh^2/2 \\ huv \end{bmatrix}
 *   G = \begin{bmatrix} hv \\ huv \\ h^2 v + gh^2/2 \end{bmatrix}
 * \]
 * Note that we also need a bound on the characteristic wave speeds
 * for the problem in order to ensure that our method doesn't explode;
 * we use this to control the Courant-Friedrichs-Levy (CFL) number
 * relating wave speeds, time steps, and space steps.
 */

struct Shallow2D {

    typedef float real;
    typedef std::array<real,3> vec;

    static constexpr real g = 9.8;  // Gravitational force

    static void F(vec& FU, const vec& U) {
        real h = U[0], hu = U[1], hv = U[2];
        FU[0] = hu;
        FU[1] = hu*hu/h + (0.5*g)*h*h;
        FU[2] = hu*hv/h;
    }

    static void G(vec& FU, const vec& U) {
        real h = U[0], hu = U[1], hv = U[2];
        FU[0] = hv;
        FU[1] = hu*hv/h;
        FU[2] = hv*hv/h + (0.5*g)*h*h;
    }

    static void wave_speed(real& cx, real& cy, const vec& U) {
        real h = U[0], hu = U[1], hv = U[2];
        real root_gh = sqrt(g * h);
        cx = abs(hu/h) + root_gh;
        cy = abs(hv/h) + root_gh;
    }
};


/********************************************************************
 * # Jiang-Tadmor central difference scheme
 *
 * Jiang and Tadmor proposed a high-resolution finite difference
 * scheme for solving hyperbolic PDE systems in two space dimensions.
 * The method is particularly attractive because, unlike many other
 * methods in this space, it does not require that we write any
 * solvers for problems involving F and G with special initial data
 * (so-called Riemann problems), nor even that we compute Jacobians
 * of F and G.  The scheme is still somewhat complicated, partly
 * because of the existence of a "limiter" (required for stability
 * for almost any nonlinear hyperbolic PDE solver).  Nonetheless,
 * it is much simpler than the alternatives!
 *
 * The Jiang-Tadmor scheme works by alternating between two staggered
 * grids.  We currently manage this implicitly: location (i,j) in the
 * solution
 *
 * I note that Jiang and Tadmor use a different set of hyperbolic PDEs,
 * the Euler equations, instead of the shallow water equations.  If I've
 * done my job right, there should be no need to change the code below to
 * accomodate the Euler equations; the only thing that changes is the
 * implementation of the physics class.
 *
 * Ref:
 * http://www.cscamm.umd.edu/tadmor/pub/central-schemes/Jiang-Tadmor.SISSC-98.pdf
 */
template <class Physics>
class Central2D {
public:
    typedef typename Physics::real real;
    typedef typename Physics::vec  vec;

    Central2D(real w, real h, int nx, int ny,
              real cfl = 0.2, real theta = 1.0) :
        nx(nx), ny(ny),
        nx_all(nx + 2*nghost),
        ny_all(ny + 2*nghost),
        dx(w/nx), dy(h/ny),
        cfl(cfl), theta(theta),
        u_ (nx_all * ny_all),
        f_ (nx_all * ny_all),
        g_ (nx_all * ny_all),
        ux_(nx_all * ny_all),
        uy_(nx_all * ny_all),
        fx_(nx_all * ny_all),
        gy_(nx_all * ny_all),
        v_ (nx_all * ny_all) {}

    void run(real tfinal);

    template <typename F>
    void init(F f);

    template <typename F>
    void write_pgm(const char* name, F f);

    // Diagnostics

    void solution_check();

private:
    static constexpr int nghost = 3;   // Number of ghost cells

    const int nx, ny;          // Number of (non-ghost) cells in x/y
    const int nx_all, ny_all;  // Total cells in x/y (including ghost)
    const real dx, dy;         // Cell size in x/y
    const real theta;          // Parameter for minmod limiter
    const real cfl;            // Allowed CFL number

    vector<vec> u_;            // Solution values
    vector<vec> f_;            // Fluxes in x
    vector<vec> g_;            // Fluxes in y
    vector<vec> ux_;           // x differences of u
    vector<vec> uy_;           // y differences of u
    vector<vec> fx_;           // x differences of f
    vector<vec> gy_;           // y differences of g
    vector<vec> v_;            // Solution values at next step

    // Array accessor functions

    int offset(int ix, int iy)  { return iy*nx_all+ix; }

    vec& u(int ix, int iy)    { return u_[offset(ix,iy)]; }
    vec& v(int ix, int iy)    { return v_[offset(ix,iy)]; }
    vec& f(int ix, int iy)    { return f_[offset(ix,iy)]; }
    vec& g(int ix, int iy)    { return g_[offset(ix,iy)]; }

    vec& ux(int ix, int iy)   { return ux_[offset(ix,iy)]; }
    vec& uy(int ix, int iy)   { return uy_[offset(ix,iy)]; }
    vec& fx(int ix, int iy)   { return fx_[offset(ix,iy)]; }
    vec& gy(int ix, int iy)   { return gy_[offset(ix,iy)]; }

    // Wrapped accessor (periodic BC)
    int ioffset(int ix, int iy) {
        return offset( (ix+nx-nghost) % nx + nghost,
                       (iy+ny-nghost) % ny + nghost );
    }

    vec& uwrap(int ix, int iy)  { return u_[ioffset(ix,iy)]; }

    // Differencing with minmod limiter

    real xmin(real a, real b) {
        return (( copysign((real) 0.5, a) + copysign((real) 0.5, b) ) *
                min( abs(a), abs(b) ));
    }

    real xmic(real du1, real du2) {
        return xmin( theta*xmin(du1, du2), 0.5*(du1+du2) );
    }

    real limdiff(real um, real u0, real up) {
        return xmic(u0-um, up-u0);
    }

    void limdiff(vec& du, const vec& um, const vec& u0, const vec& up) {
        for (int m = 0; m < du.size(); ++m)
            du[m] = limdiff(um[m], u0[m], up[m]);
    }

    // Stages of the main algorithm

    void apply_periodic();
    void compute_fg_speeds(real& cx, real& cy);
    void limited_derivs();
    void compute_step(int io, real dt);

};


/*
 * Apply periodic boundary conditions by copying ghost cell data
 * We assume the range [nghost, nx+nghost]-by-[nghost, ny+nghost] has the
 * "canonical" versions of the cell values, and other cells should
 * be overwritten.
 */
template <class Physics>
void Central2D<Physics>::apply_periodic()
{
    // Copy data between right and left boundaries
    for (int iy = 0; iy < ny_all; ++iy)
        for (int ix = 0; ix < nghost; ++ix) {
            u(ix,          iy) = uwrap(ix,          iy);
            u(nx+nghost+ix,iy) = uwrap(nx+nghost+ix,iy);
        }

    // Copy data between top and bottom boundaries
    for (int ix = 0; ix < nx_all; ++ix)
        for (int iy = 0; iy < nghost; ++iy) {
            u(ix,          iy) = uwrap(ix,          iy);
            u(ix,ny+nghost+iy) = uwrap(ix,ny+nghost+iy);
        }
}


/*
 * Evaluate F and G at the cell centers at the start of the step.
 * Also compute (bounds) on the characteristic wave speeds
 * as the basis for later computation of the CFL condition.
 */
template <class Physics>
void Central2D<Physics>::compute_fg_speeds(real& cx_, real& cy_)
{
    real cx = 1.0e-15;
    real cy = 1.0e-15;
    for (int iy = 0; iy < ny_all; ++iy)
        for (int ix = 0; ix < nx_all; ++ix) {
            real cell_cx, cell_cy;
            Physics::F(f(ix,iy), u(ix,iy));
            Physics::G(g(ix,iy), u(ix,iy));
            Physics::wave_speed(cell_cx, cell_cy, u(ix,iy));
            cx = max(cx, cell_cx);
            cy = max(cy, cell_cy);
        }
    cx_ = cx;
    cy_ = cy;
}


/*
 * Compute differences of F in the x direction, G in the y direction,
 * and u in both directions.  In order to maintain stability, we
 * need to use a slope limiter for these computations.
 */
template <class Physics>
void Central2D<Physics>::limited_derivs()
{
    for (int iy = 1; iy < ny_all-1; ++iy)
        for (int ix = 1; ix < nx_all-1; ++ix) {

            // x derivs
            limdiff( ux(ix,iy), u(ix-1,iy), u(ix,iy), u(ix+1,iy) );
            limdiff( fx(ix,iy), f(ix-1,iy), f(ix,iy), f(ix+1,iy) );

            // y derivs
            limdiff( uy(ix,iy), u(ix,iy-1), u(ix,iy), u(ix,iy+1) );
            limdiff( gy(ix,iy), g(ix,iy-1), g(ix,iy), g(ix,iy+1) );
        }
}


/*
 * Take one step of the numerical scheme.  This consists of two pieces:
 * a first-order corrector computed at a half time step, which is used
 * to obtain new F and G values; and a corrector step that computes
 * the solution at the full step.
 */
template <class Physics>
void Central2D<Physics>::compute_step(int io, real dt)
{
    real dtcdx2 = 0.5 * dt / dx;
    real dtcdy2 = 0.5 * dt / dy;

    // Predictor (flux values of f and g at half step)
    for (int iy = 1; iy < ny_all-1; ++iy)
        for (int ix = 1; ix < nx_all-1; ++ix) {
            vec uh = u(ix,iy);
            for (int m = 0; m < uh.size(); ++m) {
                uh[m] -= dtcdx2 * fx(ix,iy)[m];
                uh[m] -= dtcdy2 * gy(ix,iy)[m];
            }
            Physics::F(f(ix,iy), uh);
            Physics::G(g(ix,iy), uh);
        }

    // Corrector (finish the step)
    for (int iy = nghost-io; iy < ny+nghost-io; ++iy)
        for (int ix = nghost-io; ix < nx+nghost-io; ++ix) {
            for (int m = 0; m < v(ix,iy).size(); ++m) {
                v(ix,iy)[m] =
                    0.2500 * ( u(ix,  iy)[m] + u(ix+1,iy  )[m] +
                               u(ix,iy+1)[m] + u(ix+1,iy+1)[m] ) -
                    0.0625 * ( ux(ix+1,iy  )[m] - ux(ix,iy  )[m] +
                               ux(ix+1,iy+1)[m] - ux(ix,iy+1)[m] +
                               uy(ix,  iy+1)[m] - uy(ix,  iy)[m] +
                               uy(ix+1,iy+1)[m] - uy(ix+1,iy)[m] ) -
                    dtcdx2 * ( f(ix+1,iy  )[m] - f(ix,iy  )[m] +
                               f(ix+1,iy+1)[m] - f(ix,iy+1)[m] ) -
                    dtcdy2 * ( g(ix,  iy+1)[m] - g(ix,  iy)[m] +
                               g(ix+1,iy+1)[m] - g(ix+1,iy)[m] );
            }
        }

    // Copy from v storage back to main grid
    for (int j = nghost; j < ny+nghost; ++j)
        for (int i = nghost; i < nx+nghost; ++i)
            u(i,j) = v(i-io,j-io);
}


/*
 * Run the method forward from time 0 (initial conditions) to time tfinal.
 * We ensure that we take an even number of steps so that the solution
 * at the end lives on the main grid instead of the staggered grid.
 */
template <class Physics>
void Central2D<Physics>::run(real tfinal)
{
    bool done = false;
    real t = 0;
    while (!done) {
        real dt;
        for (int io = 0; io < 2; ++io) {
            real cx, cy;
            apply_periodic();
            solution_check();
            compute_fg_speeds(cx, cy);
            limited_derivs();
            if (io == 0) {
                dt = cfl / max(cx/dx, cy/dy);
                if (t + 2*dt >= tfinal) {
                    dt = (tfinal-t)/2;
                    done = true;
                }
            }
            compute_step(io, dt);
            t += dt;
        }
    }
}


/*
 * The numerical method is supposed to preserve (up to rounding errors)
 * the total volume of water in the domain and the total momentum.
 * Ideally, we should also not see negative water heights, since that will
 * cause the system of equations to blow up.  For debugging convenience,
 * we'll plan to print diagnostic information about these conserved quantities
 * (and about the range of water heights) at every step.
 */
template <class Physics>
void Central2D<Physics>::solution_check()
{
    real h_sum = 0, hu_sum = 0, hv_sum = 0;
    real hmin = u(nghost,nghost)[0];
    real hmax = hmin;
    for (int j = nghost; j < ny+nghost; ++j)
        for (int i = nghost; i < nx+nghost; ++i) {
            vec& uij = u(i,j);
            real h = uij[0];
            h_sum += h;
            hu_sum += uij[1];
            hv_sum += uij[2];
            hmax = max(h, hmax);
            hmin = min(h, hmin);
            assert( h > 0) ;
        }
    real cell_area = dx*dy;
    h_sum *= cell_area;
    hu_sum *= cell_area;
    hv_sum *= cell_area;
    printf("%g volume; (%g, %g) momentum; range [%g, %g]\n",
           h_sum, hu_sum, hv_sum, hmin, hmax);
}


/*
 * The Portable Gray Map (PGM) format is one of the few graphics formats
 * that can be dumped out in a handful of lines of code without any library
 * calls.  The files can be converted to something more modern and snazzy
 * (like a PNG or GIF) later on.  Note that we don't actually dump out
 * the state vector for each cell -- we need to produce something that
 * is an integer in the range [0,255].  That's what the function f is for!
 */
template <class Physics>
template <typename F>
void Central2D<Physics>::write_pgm(const char* fname, F f)
{
    FILE* fp = fopen(fname, "wb");
    fprintf(fp, "P5\n");
    fprintf(fp, "%d %d 255\n", nx, ny);
    for (int iy = ny-1; iy >= 0; --iy)
        for (int ix = 0; ix < nx; ++ix)
            fputc(min(255, max(0, f(u(ix+nghost,iy+nghost)))), fp);
    fclose(fp);
}


/*
 * Initialize the mesh by calling some function f on each set of
 * mesh coordinates.  The function f has the signature
 *   f(uxy, x, y)
 * where uxy is an output variable.
 */
template <class Physics>
template <typename F>
void Central2D<Physics>::init(F f)
{
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix)
            f(u(nghost+ix,nghost+iy), (ix+0.5)*dx, (iy+0.5)*dy);
}


/********************************************************************
 * # Initial states and graphics
 *
 * Ideally, I would be doing this (and the write_pgm above) via
 * a Python interface.  But I couldn't be bothered to deal with
 * the linker.
 */

/*
 * Initial conditions for a circular dam break problem
 */
void dam_break(Central2D<Shallow2D>::vec& u, double x, double y)
{
    x -= 1;
    y -= 1;
    u[0] = 1.0 + 0.5*(x*x + y*y < 0.25+1e-5);
    u[1] = 0;
    u[2] = 0;
}


/*
 * Initial conditions for a still pond
 */
void pond(Central2D<Shallow2D>::vec& u, double x, double y)
{
    u[0] = 1.0;
    u[1] = 0;
    u[2] = 0;
}


/*
 * Function to plot the height (max value assumed 3.0)
 */
int show_height(const Central2D<Shallow2D>::vec& u)
{
    return 255 * (u[0] / 3.0);
}


/*
 * Function to plot momentum
 */
int show_momentum(const Central2D<Shallow2D>::vec& u)
{
    return 255 * sqrt(u[1]*u[1] + u[2]*u[2]) / 2.5;
}


/********************************************************************
 * # Main driver
 *
 * Again, this should really invoke an option parser, or be glued
 * to an interface in some appropriate scripting language (Python,
 * or perhaps Lua).
 */

int main()
{
    Central2D<Shallow2D> sim(2,2, 200,200, 0.2, 2.0);
    sim.init(dam_break);
    sim.solution_check();
    sim.write_pgm("test.pgm", show_height);
    sim.run(0.5);
    sim.write_pgm("test2.pgm", show_height);
}