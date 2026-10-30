// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Optimization of pop with struct types
// Codegen for TestByRef and TestByPtr should be similar
//
// See https://github.com/dotnet/runtime/issues/10607

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

[StructLayout(LayoutKind.Sequential)]
struct VT
{
    public static readonly int Size = Marshal.SizeOf<VT>();
    
    public int F1, F2, F3;
    
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Get(out int v1, out int v2, out int v3)
    {
        v1 = F1;
        v2 = F2;
        v3 = F3;
    }
}

public class P
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    unsafe static int TestMethodInlining(VT* pVT)
    {
        int v1, v2, v3;
        pVT->Get(out v1, out v2, out v3);
        return Do(v1, v2);
    }
    
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int TestByRef(ref VT VTRef)
    {
        int v1, v2, v3;
        v1 = VTRef.F1;
        v2 = VTRef.F2;
        v3 = VTRef.F3;
        return Do(v1, v2);
    }
    
    [MethodImpl(MethodImplOptions.NoInlining)]
    unsafe static int TestByPtr(VT* pVT)
    {
        int v1, v2, v3;
        v1 = pVT->F1;
        v2 = pVT->F2;
        v3 = pVT->F3;
        return Do(v1, v2);
    }
    
    [Fact]
    public unsafe static int TestEntryPoint()
    {
        byte* pDataBytes = stackalloc byte[VT.Size];
        VT* pVT = (VT*)pDataBytes;
        pVT->F1 = 44;
        pVT->F2 = 56;
        pVT->F3 = 3;

        int result = -200;
        result += TestMethodInlining(pVT);
        result += TestByRef(ref *pVT);
        result += TestByPtr(pVT);

        return result;
    }
    
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Do(int v1, int v2)
    {
        return v1 + v2;
    }

    private static int s_trace;

    [Fact]
    public static void UnreadAddressStores()
    {
        s_trace = 0;
        Assert.Equal(7, UnreadAddressStore(7));
        Assert.Equal(3, s_trace);
        Assert.Equal(10, UnreadSpanAddressStore(new[] { 7, 8, 9 }));
        Assert.Equal(34, s_trace);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Touch(int digit)
    {
        s_trace = s_trace * 10 + digit;
        return digit;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool Never() => false;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int UnreadAddressStore(int value)
    {
        ref int alias = ref value;
        if (Never())
            alias++;
        Touch(3);
        return value;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int UnreadSpanAddressStore(int[] values)
    {
        Span<int> span = values;
        ref Span<int> alias = ref span;
        if (Never())
            alias = default;
        // Once the branch folds away, the unread alias must not expose the span.
        Touch(4);
        return span.Length + span[0];
    }

}


