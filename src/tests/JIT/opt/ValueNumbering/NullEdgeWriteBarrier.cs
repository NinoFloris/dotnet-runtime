// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using Xunit;

public class NullEdgeWriteBarrier
{
    private sealed class Holder
    {
        public object Value;
    }

    private static int s_marker;

    [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.AggressiveOptimization)]
    private static void StoreEqual(Holder target, object value)
    {
        if (value == null)
        {
            s_marker = 1;
            target.Value = value;
            s_marker = 2;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.AggressiveOptimization)]
    private static void StoreNotEqual(Holder target, object value)
    {
        if (value != null)
            return;

        s_marker = 1;
        target.Value = value;
        s_marker = 2;
    }

    [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.AggressiveOptimization)]
    private static void StoreAtJoin(Holder target, object value)
    {
        if (value == null)
            s_marker = 1;
        else
            s_marker = 3;

        target.Value = value;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference StoreYoungValue(Holder target)
    {
        object value = new object();
        var weak = new WeakReference(value);
        StoreAtJoin(target, value);
        return weak;
    }

    [Fact]
    public static void NullEdgesPreserveStoresAndFaults()
    {
        var target = new Holder { Value = new object() };
        StoreEqual(target, null);
        Assert.Null(target.Value);
        Assert.Equal(2, s_marker);

        target.Value = new object();
        StoreNotEqual(target, null);
        Assert.Null(target.Value);
        Assert.Equal(2, s_marker);

        s_marker = 0;
        Assert.Throws<NullReferenceException>(() => StoreEqual(null, null));
        Assert.Equal(1, s_marker);
        s_marker = 0;
        Assert.Throws<NullReferenceException>(() => StoreNotEqual(null, null));
        Assert.Equal(1, s_marker);
    }

    [Fact]
    public static void NonNullEntryStillNeedsBarrier()
    {
        var target = new Holder();
        GC.Collect(2, GCCollectionMode.Forced, blocking: true);
        WeakReference weak = StoreYoungValue(target);
        GC.Collect(0, GCCollectionMode.Forced, blocking: true);
        Assert.True(weak.IsAlive);
        Assert.Same(weak.Target, target.Value);
        GC.KeepAlive(target);
    }
}
