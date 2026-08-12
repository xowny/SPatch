[CmdletBinding()]
param(
    [switch] $NoExit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Deterministic CPU validator for the PBR helper in
# luma/overlay/Shaders/Sleeping Dogs Definitive Edition/SPatchPBR.hlsl.
#
# Validated domains: the opaque deferred-light helpers use native Blinn-Phong
# exponents through 1024, while exact vehicle glass 0x282EE2DC legitimately
# reaches 32767. The HLSL now evaluates the algebraically identical alpha^2 =
# 2 / (exponent + 2) directly; this validator cross-checks that reduction
# against the former pow/square chain before integrating the GGX BRDF.
#
# Directional hemispherical reflectance is estimated with 131072 deterministic
# Hammersley samples of the GGX NDF for each view/material/exponent tuple.
# Sampling half vectors, then reflecting V around H, concentrates samples in
# the narrow lobe at the smoothest permitted roughness.  Invalid reflected
# directions contribute zero.  The Lambert interface-transmission term is
# integrated analytically (the exact azimuthally symmetric integral), avoiding
# unnecessary quadrature noise. This is bounded work: 11 materials * 10
# exponents * 6 view angles * 131072 samples.
#
# Gates are deliberately strict: every evaluated quantity must be finite and
# non-negative; reciprocal BRDF pairs differ by at most 2e-12; directional
# hemispherical reflectance may not exceed 1 + 0.002.  The 0.2% allowance is
# solely deterministic finite-sample integration error, not a material-energy
# adjustment.  Do not raise it to mask a failed physical case.

$source = @'
using System;
using System.Collections.Generic;
using System.Globalization;

public static class PbrBrdfNumericValidator
{
    const double Pi = Math.PI;
    const int Samples = 131072;
    const double EnergyTolerance = 0.002;
    const double ReciprocityTolerance = 2e-12;

    struct V3
    {
        public double X, Y, Z;
        public V3(double x, double y, double z) { X = x; Y = y; Z = z; }
        public static V3 operator +(V3 a, V3 b) { return new V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
        public static V3 operator -(V3 a, V3 b) { return new V3(a.X - b.X, a.Y - b.Y, a.Z - b.Z); }
        public static V3 operator *(V3 a, double b) { return new V3(a.X * b, a.Y * b, a.Z * b); }
        public static V3 operator *(double b, V3 a) { return a * b; }
        public static V3 operator *(V3 a, V3 b) { return new V3(a.X * b.X, a.Y * b.Y, a.Z * b.Z); }
    }

    sealed class Material
    {
        public readonly string Name;
        public readonly V3 F0;
        public readonly V3 F90;
        public readonly double Metallic;
        public Material(string name, V3 f0, double metallic, bool unitGrazing)
        {
            Name = name;
            F0 = f0;
            F90 = unitGrazing
                ? new V3(1.0, 1.0, 1.0)
                : new V3(Math.Min(1.0, 50.0 * f0.X), Math.Min(1.0, 50.0 * f0.Y), Math.Min(1.0, 50.0 * f0.Z));
            Metallic = metallic;
        }
    }

    sealed class State
    {
        public bool Finite = true;
        public double Minimum = 0.0;
        public double MaxReciprocity = 0.0;
        public string MaxReciprocityCase = "n/a";
        public double MaxReflectance = Double.NegativeInfinity;
        public string MaxReflectanceCase = "n/a";
        public int EnergyCases;
        public int ReciprocityCases;
        public int EnvironmentCases;
        public double MaxEnvironmentPartitionError = 0.0;
        public double MaxEnvironmentConvexityError = 0.0;
        public int VehiclePaintEnvironmentCases;
        public double MaxVehiclePaintPartitionError = 0.0;
        public double MaxVehiclePaintConvexityError = 0.0;

        public void Check(double value)
        {
            if (Double.IsNaN(value) || Double.IsInfinity(value)) { Finite = false; return; }
            if (value < Minimum) Minimum = value;
        }
        public void Check(V3 value) { Check(value.X); Check(value.Y); Check(value.Z); }
    }

    static double Saturate(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
    static double Dot(V3 a, V3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    static double Length(V3 a) { return Math.Sqrt(Dot(a, a)); }
    static V3 Normalize(V3 a) { return a * (1.0 / Length(a)); }
    static double MaxComponent(V3 a) { return Math.Max(a.X, Math.Max(a.Y, a.Z)); }
    static double MaxAbsDifference(V3 a, V3 b)
    {
        return Math.Max(Math.Abs(a.X - b.X), Math.Max(Math.Abs(a.Y - b.Y), Math.Abs(a.Z - b.Z)));
    }

    // Matches the explicit native-family F90 Schlick helper.
    static V3 Fresnel(double cosTheta, V3 f0, V3 f90)
    {
        double t = Math.Pow(1.0 - Saturate(cosTheta), 5.0);
        return f0 + (f90 - f0) * t;
    }

    static double FormerAlphaSquared(double nativeExponent)
    {
        double roughness = Math.Max(Math.Pow(2.0 / (nativeExponent + 2.0), 0.25), 0.045);
        double alpha = roughness * roughness;
        return alpha * alpha;
    }

    static V3 HalfVector(V3 v, V3 l)
    {
        V3 sum = v + l;
        return Normalize(sum);
    }

    // Physical BRDF-domain equivalent of SPatchPBRDirectSpecular. The HLSL
    // cancels NDF /Pi against the engine's Lambert-light Pi; this reference
    // retains /Pi so directional hemispherical reflectance stays in physical
    // units. It excludes the NoL factor applied by the calling shaders.
    static V3 Specular(V3 normal, V3 v, V3 l, double nativeExponent, Material material, State state)
    {
        double noV = Saturate(Dot(normal, v));
        double noL = Saturate(Dot(normal, l));
        V3 h = HalfVector(v, l);
        double noH = Saturate(Dot(normal, h));
        double alphaSquared = 2.0 / (nativeExponent + 2.0);
        double denominator = noH * noH * (alphaSquared - 1.0) + 1.0;
        double distribution = alphaSquared / (Pi * denominator * denominator);
        double smithV = noL * Math.Sqrt(noV * noV * (1.0 - alphaSquared) + alphaSquared);
        double smithL = noV * Math.Sqrt(noL * noL * (1.0 - alphaSquared) + alphaSquared);
        double visibility = 0.5 / Math.Max(smithV + smithL, 1.0e-6);
        V3 result = Fresnel(Dot(v, h), material.F0, material.F90) * (distribution * visibility);
        state.Check(result);
        return result;
    }

    // Mirrors SPatchPBRDiffuseWeight with the same native-family F90.
    static V3 DiffuseWeight(V3 normal, V3 v, V3 l, Material material, State state)
    {
        V3 one = new V3(1.0, 1.0, 1.0);
        V3 result = (one - Fresnel(Dot(normal, v), material.F0, material.F90))
            * (one - Fresnel(Dot(normal, l), material.F0, material.F90))
            * (1.0 - material.Metallic);
        state.Check(result);
        return result;
    }

    static V3 Brdf(V3 normal, V3 v, V3 l, double nativeExponent, Material material, State state)
    {
        V3 specular = Specular(normal, v, l, nativeExponent, material, state);
        V3 diffuse = DiffuseWeight(normal, v, l, material, state) * (1.0 / Pi);
        V3 result = specular + diffuse;
        state.Check(result);
        return result;
    }

    static V3 GlassEnvironmentComposite(
        V3 bodyLighting, V3 environmentRadiance, double noV, State state)
    {
        V3 one = new V3(1.0, 1.0, 1.0);
        V3 f0 = new V3(0.05, 0.05, 0.05);
        V3 fresnel = Fresnel(noV, f0, one);
        V3 transmission = one - fresnel;
        V3 result = bodyLighting * transmission +
            environmentRadiance * fresnel;
        state.Check(fresnel);
        state.Check(transmission);
        state.Check(result);
        state.MaxEnvironmentPartitionError = Math.Max(
            state.MaxEnvironmentPartitionError,
            MaxAbsDifference(transmission + fresnel, one));
        state.EnvironmentCases++;
        return result;
    }

    static V3 VehiclePaintEnvironmentComposite(
        V3 bodyLighting, V3 environmentRadiance, double noV, double scalarF0,
        State state)
    {
        V3 one = new V3(1.0, 1.0, 1.0);
        V3 f0 = new V3(scalarF0, scalarF0, scalarF0);
        double scalarF90 = Math.Min(1.0, 50.0 * scalarF0);
        V3 f90 = new V3(scalarF90, scalarF90, scalarF90);
        V3 fresnel = Fresnel(noV, f0, f90);
        V3 transmission = one - fresnel;
        V3 result = bodyLighting * transmission +
            environmentRadiance * fresnel;
        state.Check(fresnel);
        state.Check(transmission);
        state.Check(result);
        state.MaxVehiclePaintPartitionError = Math.Max(
            state.MaxVehiclePaintPartitionError,
            MaxAbsDifference(transmission + fresnel, one));
        state.VehiclePaintEnvironmentCases++;
        return result;
    }

    static uint ReverseBits(uint value)
    {
        value = (value << 16) | (value >> 16);
        value = ((value & 0x00ff00ffU) << 8) | ((value & 0xff00ff00U) >> 8);
        value = ((value & 0x0f0f0f0fU) << 4) | ((value & 0xf0f0f0f0U) >> 4);
        value = ((value & 0x33333333U) << 2) | ((value & 0xccccccccU) >> 2);
        return ((value & 0x55555555U) << 1) | ((value & 0xaaaaaaaaU) >> 1);
    }

    static V3 SampleGgxNdf(double u1, double u2, double alpha)
    {
        // Inverse CDF for p(H)=D(NoH)*NoH.  Midpoints avoid u=0/1 poles.
        double alphaSquared = alpha * alpha;
        double cosTheta = Math.Sqrt((1.0 - u1) / (1.0 + (alphaSquared - 1.0) * u1));
        double sinTheta = Math.Sqrt(Math.Max(0.0, 1.0 - cosTheta * cosTheta));
        double phi = 2.0 * Pi * u2;
        return new V3(sinTheta * Math.Cos(phi), sinTheta * Math.Sin(phi), cosTheta);
    }

    static V3 ViewDirection(double noV)
    {
        return new V3(Math.Sqrt(Math.Max(0.0, 1.0 - noV * noV)), 0.0, noV);
    }

    static V3 Direction(double noZ, double phi)
    {
        double sinTheta = Math.Sqrt(Math.Max(0.0, 1.0 - noZ * noZ));
        return new V3(sinTheta * Math.Cos(phi), sinTheta * Math.Sin(phi), noZ);
    }

    static V3 DiffuseDirectionalReflectance(double noV, Material material)
    {
        // For F=F0+(F90-F0)(1-u)^5, 2*integral(u*(1-F(u)),u=0..1)
        // is (1-F0) - (F90-F0)/21, channel-wise.
        V3 one = new V3(1.0, 1.0, 1.0);
        V3 viewTransmission = one - Fresnel(noV, material.F0, material.F90);
        V3 lightTransmissionIntegral = (one - material.F0)
            - (material.F90 - material.F0) * (1.0 / 21.0);
        return viewTransmission * lightTransmissionIntegral * (1.0 - material.Metallic);
    }

    static V3 SpecularDirectionalReflectance(V3 normal, V3 v, double nativeExponent, Material material, State state)
    {
        double alphaSquared = 2.0 / (nativeExponent + 2.0);
        double alpha = Math.Sqrt(alphaSquared);
        V3 integral = new V3(0.0, 0.0, 0.0);
        for (uint i = 0; i < Samples; ++i)
        {
            double u1 = ((double)i + 0.5) / Samples;
            double u2 = ((double)ReverseBits(i) + 0.5) / 4294967296.0;
            V3 h = SampleGgxNdf(u1, u2, alpha);
            double voH = Dot(v, h);
            V3 l = h * (2.0 * voH) - v;
            double noL = Dot(normal, l);
            if (voH <= 0.0 || noL <= 0.0) continue;

            double noH = h.Z;
            double denominator = noH * noH * (alphaSquared - 1.0) + 1.0;
            double distribution = alphaSquared / (Pi * denominator * denominator);
            double pdfL = distribution * noH / (4.0 * voH);
            V3 estimate = Specular(normal, v, l, nativeExponent, material, state) * (noL / pdfL);
            state.Check(estimate);
            integral = integral + estimate;
        }
        return integral * (1.0 / Samples);
    }

    static void ValidateReciprocity(V3 normal, double[] noValues, Material[] materials, double[] exponents, State state)
    {
        double[] phis = { 0.0, 0.713, 2.147 };
        foreach (Material material in materials)
        foreach (double exponent in exponents)
        foreach (double noV in noValues)
        foreach (double noL in noValues)
        foreach (double phiV in phis)
        foreach (double phiL in phis)
        {
            V3 v = Direction(noV, phiV);
            V3 l = Direction(noL, phiL);
            V3 forward = Brdf(normal, v, l, exponent, material, state);
            V3 reverse = Brdf(normal, l, v, exponent, material, state);
            double difference = MaxAbsDifference(forward, reverse);
            if (difference > state.MaxReciprocity)
            {
                state.MaxReciprocity = difference;
                state.MaxReciprocityCase = String.Format(CultureInfo.InvariantCulture,
                    "material={0}, exponent={1:G}, NoV={2:G4}, NoL={3:G4}, phiV={4:G4}, phiL={5:G4}",
                    material.Name, exponent, noV, noL, phiV, phiL);
            }
            state.ReciprocityCases++;
        }
    }

    public static int Run()
    {
        V3 normal = new V3(0.0, 0.0, 1.0);
        double[] exponents = { 0.0, 1.0, 8.0, 32.0, 128.0, 512.0, 1024.0, 8191.0, 16383.0, 32767.0 };
        double[] noValues = { 0.02, 0.10, 0.30, 0.60, 0.90, 0.999 };
        Material[] materials = {
            new Material("foliage-zero-f0-native-f90", new V3(0.0, 0.0, 0.0), 0.0, false),
            new Material("volume-zero-f0-unit-f90", new V3(0.0, 0.0, 0.0), 0.0, true),
            new Material("opaque-low-f0-0.005-native-f90", new V3(0.005, 0.005, 0.005), 0.0, false),
            new Material("opaque-low-f0-0.019-native-f90", new V3(0.019, 0.019, 0.019), 0.0, false),
            new Material("dielectric-f0-0.02", new V3(0.02, 0.02, 0.02), 0.0, false),
            new Material("dielectric-f0-0.04", new V3(0.04, 0.04, 0.04), 0.0, false),
            new Material("dielectric-f0-0.05-glass", new V3(0.05, 0.05, 0.05), 0.0, true),
            new Material("dielectric-f0-0.08", new V3(0.08, 0.08, 0.08), 0.0, false),
            new Material("dielectric-colored-f90-ramp", new V3(0.01, 0.02, 0.08), 0.0, false),
            new Material("metal-copper", new V3(0.955, 0.638, 0.538), 1.0, false),
            new Material("metal-silver", new V3(0.972, 0.960, 0.915), 1.0, false)
        };
        double maxAlphaReductionError = 0.0;
        foreach (double exponent in new double[] { 0.0, 1.0, 8.0, 32.0, 128.0, 512.0, 1024.0, 4096.0, 32767.0, 65536.0 })
        {
            maxAlphaReductionError = Math.Max(maxAlphaReductionError,
                Math.Abs(FormerAlphaSquared(exponent) - 2.0 / (exponent + 2.0)));
        }
        V3 zero = new V3(0.0, 0.0, 0.0);
        V3 one = new V3(1.0, 1.0, 1.0);
        Material nativeRamp = new Material("native-f90-ramp-contract",
            new V3(0.005, 0.010, 0.019), 0.0, false);
        Material nativeSaturation = new Material("native-f90-saturation-contract",
            new V3(0.020, 0.021, 1.0), 0.0, false);
        Material unitGrazing = new Material("unit-f90-contract",
            new V3(0.0, 0.01, 0.5), 0.0, true);
        double f90ContractError = Math.Max(
            Math.Max(
                MaxComponent(Fresnel(0.0, zero, zero)),
                MaxAbsDifference(Fresnel(0.0, zero, one), one)),
            Math.Max(
                MaxAbsDifference(nativeRamp.F90, new V3(0.25, 0.50, 0.95)),
                Math.Max(
                    MaxAbsDifference(nativeSaturation.F90, one),
                    MaxAbsDifference(unitGrazing.F90, one))));
        State state = new State();

        double glassEnvironmentContractError = Math.Max(
            MaxAbsDifference(
                Fresnel(1.0, new V3(0.05, 0.05, 0.05), one),
                new V3(0.05, 0.05, 0.05)),
            MaxAbsDifference(
                Fresnel(0.0, new V3(0.05, 0.05, 0.05), one),
                one));
        V3[] environmentValues = {
            new V3(0.0, 0.0, 0.0),
            new V3(0.05, 0.25, 0.75),
            new V3(1.0, 1.0, 1.0)
        };
        foreach (double noV in noValues)
        foreach (V3 bodyLighting in environmentValues)
        foreach (V3 environmentRadiance in environmentValues)
        {
            V3 composite = GlassEnvironmentComposite(
                bodyLighting, environmentRadiance, noV, state);
            V3 lower = new V3(
                Math.Min(bodyLighting.X, environmentRadiance.X),
                Math.Min(bodyLighting.Y, environmentRadiance.Y),
                Math.Min(bodyLighting.Z, environmentRadiance.Z));
            V3 upper = new V3(
                Math.Max(bodyLighting.X, environmentRadiance.X),
                Math.Max(bodyLighting.Y, environmentRadiance.Y),
                Math.Max(bodyLighting.Z, environmentRadiance.Z));
            state.MaxEnvironmentConvexityError = Math.Max(
                state.MaxEnvironmentConvexityError,
                Math.Max(
                    MaxComponent(lower - composite),
                    MaxComponent(composite - upper)));
        }

        double vehiclePaintEnvironmentContractError = Math.Max(
            MaxComponent(Fresnel(0.0, zero, zero)),
            Math.Max(
                MaxAbsDifference(
                    Fresnel(1.0, new V3(0.02, 0.02, 0.02), one),
                    new V3(0.02, 0.02, 0.02)),
                MaxAbsDifference(
                    Fresnel(0.0, new V3(0.02, 0.02, 0.02), one),
                    one)));
        double[] vehiclePaintF0Values = { 0.0, 0.005, 0.02, 0.04, 0.30, 1.0 };
        foreach (double noV in noValues)
        foreach (double scalarF0 in vehiclePaintF0Values)
        foreach (V3 bodyLighting in environmentValues)
        foreach (V3 environmentRadiance in environmentValues)
        {
            V3 composite = VehiclePaintEnvironmentComposite(
                bodyLighting, environmentRadiance, noV, scalarF0, state);
            V3 lower = new V3(
                Math.Min(bodyLighting.X, environmentRadiance.X),
                Math.Min(bodyLighting.Y, environmentRadiance.Y),
                Math.Min(bodyLighting.Z, environmentRadiance.Z));
            V3 upper = new V3(
                Math.Max(bodyLighting.X, environmentRadiance.X),
                Math.Max(bodyLighting.Y, environmentRadiance.Y),
                Math.Max(bodyLighting.Z, environmentRadiance.Z));
            state.MaxVehiclePaintConvexityError = Math.Max(
                state.MaxVehiclePaintConvexityError,
                Math.Max(
                    MaxComponent(lower - composite),
                    MaxComponent(composite - upper)));
        }

        ValidateReciprocity(normal, noValues, materials, exponents, state);
        foreach (Material material in materials)
        foreach (double exponent in exponents)
        foreach (double noV in noValues)
        {
            V3 v = ViewDirection(noV);
            V3 reflectance = SpecularDirectionalReflectance(normal, v, exponent, material, state)
                + DiffuseDirectionalReflectance(noV, material);
            state.Check(reflectance);
            double maximum = MaxComponent(reflectance);
            if (maximum > state.MaxReflectance)
            {
                state.MaxReflectance = maximum;
                state.MaxReflectanceCase = String.Format(CultureInfo.InvariantCulture,
                    "material={0}, exponent={1:G}, NoV={2:G4}, rgb=({3:F9}, {4:F9}, {5:F9})",
                    material.Name, exponent, noV, reflectance.X, reflectance.Y, reflectance.Z);
            }
            state.EnergyCases++;
        }

        Console.WriteLine("PBR BRDF numeric validation");
        Console.WriteLine("Domains: opaque exponent [0, 1024], vehicle-glass exponent [0, 32767], alpha^2=2/(n+2), native-family F90, GGX samples={0}", Samples);
        Console.WriteLine("Worst directional reflectance: {0:F9} ({1})", state.MaxReflectance, state.MaxReflectanceCase);
        Console.WriteLine("Worst reciprocity delta: {0:E9} ({1})", state.MaxReciprocity, state.MaxReciprocityCase);
        Console.WriteLine("Minimum evaluated component: {0:E9}; energy cases={1}; reciprocity pairs={2}",
            state.Minimum, state.EnergyCases, state.ReciprocityCases);
        Console.WriteLine("Alpha^2 reduction max error: {0:E9}; F90 contract error: {1:E9}",
            maxAlphaReductionError, f90ContractError);
        Console.WriteLine("Glass environment Fresnel contract error: {0:E9}; partition error: {1:E9}; convexity error: {2:E9}; cases={3}",
            glassEnvironmentContractError,
            state.MaxEnvironmentPartitionError,
            state.MaxEnvironmentConvexityError,
            state.EnvironmentCases);
        Console.WriteLine("Vehicle-paint environment Fresnel contract error: {0:E9}; partition error: {1:E9}; convexity error: {2:E9}; cases={3}",
            vehiclePaintEnvironmentContractError,
            state.MaxVehiclePaintPartitionError,
            state.MaxVehiclePaintConvexityError,
            state.VehiclePaintEnvironmentCases);

        bool pass = state.Finite && state.Minimum >= 0.0
            && state.MaxReciprocity <= ReciprocityTolerance
            && state.MaxReflectance <= 1.0 + EnergyTolerance
            && maxAlphaReductionError <= 2.0e-15
            && f90ContractError == 0.0
            && glassEnvironmentContractError == 0.0
            && state.MaxEnvironmentPartitionError <= 2.0e-15
            && state.MaxEnvironmentConvexityError <= 2.0e-15
            && vehiclePaintEnvironmentContractError == 0.0
            && state.MaxVehiclePaintPartitionError <= 2.0e-15
            && state.MaxVehiclePaintConvexityError <= 2.0e-15;
        if (!pass)
        {
            Console.Error.WriteLine("FAIL: finite={0}, min={1:E9}, reciprocity limit={2:E9}, reflectance limit={3:F9}",
                state.Finite, state.Minimum, ReciprocityTolerance, 1.0 + EnergyTolerance);
            return 1;
        }
        Console.WriteLine("PASS: finite/non-negative, reciprocal, and directional reflectance <= {0:F9}", 1.0 + EnergyTolerance);
        return 0;
    }
}
'@

Add-Type -TypeDefinition $source -Language CSharp -ErrorAction Stop
$exitCode = [PbrBrdfNumericValidator]::Run()
if ($NoExit) {
    if ($exitCode -ne 0) {
        throw "PBR BRDF numeric validation failed with exit code $exitCode."
    }
    return
}
exit $exitCode
