// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using Xunit;

public class NonNegativeComparisons
{
    [Fact]
    public static void TestEntryPoint()
    {
        int[] values = { int.MinValue, -1, 0, 1, int.MaxValue };
        foreach (int value in values)
        {
            foreach (int bound in values)
            {
                // Use long comparisons for the reference result, independently of int assertion propagation.
                bool signedInRange = (long)value <= bound;
                bool unsignedInRange = (long)(uint)value <= (long)(uint)bound;
                Assert.Equal(value >= 0 && signedInRange, SignedThenUnsigned(value, bound));
                Assert.Equal(value >= 0 && bound >= 0 && (long)(uint)value < (long)(uint)bound,
                    UnsignedThenSigned(value, bound));
                Assert.Equal(bound >= 0 && signedInRange && unsignedInRange,
                    SignedWithNonNegativeBound(value, bound));
                Assert.Equal(value >= 0 && unsignedInRange && signedInRange,
                    UnsignedWithNonNegativeValue(value, bound));
            }
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool SignedThenUnsigned(int value, int bound)
    {
        // CHECK-NOT: CORINFO_HELP_THROW
        if (value < 0 || value > bound)
            return false;
        if ((uint)value > (uint)bound)
            throw new InvalidOperationException();
        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool UnsignedThenSigned(int value, int bound)
    {
        // CHECK-NOT: CORINFO_HELP_THROW
        if (value < 0 || bound < 0 || (uint)value >= (uint)bound)
            return false;
        if (value >= bound)
            throw new InvalidOperationException();
        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool SignedWithNonNegativeBound(int value, int bound)
    {
        if (bound < 0 || value > bound)
            return false;
        return (uint)value <= (uint)bound;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool UnsignedWithNonNegativeValue(int value, int bound)
    {
        if (value < 0 || (uint)value > (uint)bound)
            return false;
        return value <= bound;
    }
}
