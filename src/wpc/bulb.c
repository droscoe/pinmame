// license:GPLv3+

#include <math.h>
#include "bulb.h"

/*-------------------
/  Bulb characteristics and precomputed LUTs
/-------------------*/
typedef struct {
  double surface; /* filament surface in m² */
  double mass;	/* filament mass in kg */
  double r0; /* resistance at 293K */
  double cool_down[BULB_T_MAX + 1]; /* precomputed cool down factor */
  double heat_factor[BULB_T_MAX + 1]; /* precomputed heat factor = 1.0 / (R * Mass * Specific Heat) */
} bulb_tLampCharacteristics;

/*-------------------
/  local variables
/-------------------*/
static struct {
  double                      t_to_p[BULB_T_MAX + 1 - 1500];
  double                      p_to_t[512];
  double                      specific_heat[BULB_T_MAX + 1];
} locals;

// Bulb characteristics, estimated by fitting ratings (U,I,P) at a supposed steady state temperature of 2700K, then validating against high FPS video
static bulb_tLampCharacteristics bulbs[BULB_MAX] = {
   { 0.000001549619403110030, 0.000000203895434417560, 1.70020865326503000 }, // #44 Bulb characteristics (6.3V, 250mA, 1.26W, 5lm)
   { 0.000000929872857822516, 0.000000087040748856477, 2.83368108877505000 }, // #47 Bulb characteristics (6.3V, 150mA, 0.95W, 6.3lm)
   { 0.000001239604749734440, 0.000000140555683560514, 2.12526081658129000 }, // #86 Bulb characteristics (6.3V, 200mA, 1.58W, 11.3lm)
   { 0.000007413493071564570, 0.000001709095572478890, 1.51222718202281000 }, // #89 Bulb characteristics (13V, 580mA, 7.54W, 75.4lm)
};

/*-------------------------------
/  Initialize all pre-computed LUTs
/-------------------------------*/
void bulb_init()
{
   // Compute filament temperature to visible emission power LUT, normalized by visible emission power at T=2700K, according 
   // to the formula from "Luminous radiation from a black body and the mechanical equivalentt of light" by W.W.Coblentz and W.B.Emerson
   for (int i=0; i<= BULB_T_MAX - 1500; i++)
   {
      double T = 1500.0 + i;
      locals.t_to_p[i] = 1.247/pow(1.0+129.05/T, 204.0) + 0.0678/pow(1.0+78.85/T, 404.0) + 0.0489/pow(1.0+23.52/T, 1004.0) + 0.0406/pow(1.0+13.67/T, 2004.0);
   }
   double P2700 = locals.t_to_p[2700 - 1500];
   for (int i=0; i<= BULB_T_MAX - 1500; i++)
   {
      locals.t_to_p[i] /= P2700;
   }
   // Compute visible emission power to filament temperature LUT, normalized for a relative power of 255 for visible emission power at T=2700K
   // For the time being we simply search in the previously created LUT
   int t_pos = 0;
   for (int i=0; i<512; i++)
   {
      double p = i / 255.0;
      while (locals.t_to_p[t_pos] < p)
         t_pos++;
      locals.p_to_t[i] = 1500 + t_pos;
   }
   for (int i=0; i <= BULB_T_MAX; i++)
   {
      double T = i;
      // Compute Tungsten specific heat (energy to temperature transfer, depending on temperature) according to forumla from "Heating-times of tungsten filament incandescent lamps" by Dulli Chandra Agrawal
      locals.specific_heat[i] = 3.0 * 45.2268 * (1.0 - 310.0 * 310.0 / (20.0 * T*T)) + (2.0 * 0.0045549 * T) + (4 * 0.000000000577874 * T*T*T);
      // Compute cooldown and heat up factor for the predefined bulbs
      for (int j=0; j<BULB_MAX; j++)
      {
         // pow(T, 5.0796) is pow(T, 4) from Stefan/Boltzmann multiplied by tungsten overall (all wavelengths) emissivity which is 0.0000664*pow(T,1.0796)
         double delta_energy = -0.00000005670374419 * bulbs[j].surface * 0.0000664 * pow(T, 5.0796);
         bulbs[j].cool_down[i] = delta_energy / (locals.specific_heat[i] * bulbs[j].mass);
         bulbs[j].heat_factor[i] = 1.0 / (bulbs[j].r0 * pow(T / 293.0, 1.215) * locals.specific_heat[i] * bulbs[j].mass);
      }
   }
}

/*-------------------------------
/  Returns relative visible emission power for a given filament temperature. Result is relative to the emission power at 2700K
/-------------------------------*/
double bulb_filament_temperature_to_emission(const double T)
{
   if (T < 1500.0) return 0;
   if (T >= BULB_T_MAX) return locals.t_to_p[BULB_T_MAX - 1500];
   return locals.t_to_p[(int)T - 1500];
   // Linear interpolation is not worth its cost
   // int lower_T = (int) T, upper_T = (int) (T + 0.5);
   // double alpha = T - lower_T;
   // return (1.0 - alpha) * locals.t_to_p[lower_T - 1500] + alpha * locals.t_to_p[upper_T - 1500];
}

/*-------------------------------
/  Returns filament temperature for a given visible emission power normalized for an emission power of 1.0 at 2700K
/-------------------------------*/
double bulb_emission_to_filament_temperature(const double p)
{
   int v = (int)p * 255;
   return v >= 512 ? locals.p_to_t[511] : locals.p_to_t[v];
}

/*-------------------------------
/  Compute cool down factor of a filament
/-------------------------------*/
double bulb_cool_down_factor(const int bulb, const double T)
{
   return bulbs[bulb].cool_down[(int) T];
}

/*-------------------------------
/  Compute cool down factor of a filament over a given period
/-------------------------------*/
double bulb_cool_down(const int bulb, double T, double duration)
{
   while (duration > 0.0)
   {
      double dt = duration > 0.001 ? 0.001 : duration;
      T += dt * bulbs[bulb].cool_down[(int) T];
      if (T <= 294.0)
      {
         return 293.0;
      }
      duration -= dt;
   }
   return T;
}

/*-------------------------------
/  Compute heat up factor of a filament under a given voltage (sum of heating and cooldown)
/-------------------------------*/
double bulb_heat_up_factor(const int bulb, const double T, const double U, const double serial_R)
{
   if (serial_R)
   {
      double R = bulbs[bulb].r0 * pow(T / 293.0, 1.215);
      double U1 = U * R / (R + serial_R);
      return U1 * U1 * bulbs[bulb].heat_factor[(int) T] + bulbs[bulb].cool_down[(int) T];
   }
   else
   {
      return U * U * bulbs[bulb].heat_factor[(int) T] + bulbs[bulb].cool_down[(int) T];
   }
}

/*-------------------------------
/  Compute temperature of a filament under a given voltage over a given period (sum of heating and cooldown)
/-------------------------------*/
double bulb_heat_up(const int bulb, double T, double duration, const double U, const double serial_R)
{
   while (duration > 0.0)
   {
      T = T < 293.0 ? 293.0 : T > BULB_T_MAX ? BULB_T_MAX : T; // Keeps T within the range of the LUT (between room temperature and melt down point)
      double energy;
      if (serial_R)
      {
         const double R = bulbs[bulb].r0 * pow(T / 293.0, 1.215);
         const double U1 = U * R / (R + serial_R);
         energy = U1 * U1 * bulbs[bulb].heat_factor[(int) T] + bulbs[bulb].cool_down[(int)T];
      }
      else
      {
         energy = U * U * bulbs[bulb].heat_factor[(int) T] + bulbs[bulb].cool_down[(int)T];
      }
      if (-10 < energy && energy < 10)
      {
         // Stable state reached since electric heat (roughly) equals radiation cool down
         return T;
      }
      double dt;
      if (energy > 1000e3)
      {
         // Initial current surge, 0.5ms integration period in order to account for the fast resistor rise that will quickly lower the current
         dt = duration > 0.0005 ? 0.0005 : duration;
      }
      else
      {
         // Ramping up, 1ms integration period
         dt = duration > 0.001 ? 0.001 : duration;
      }
      T += dt * energy;
      duration -= dt;
   }
   return T;
}
