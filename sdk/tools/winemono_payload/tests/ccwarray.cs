/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test interface arrays passed to a managed object through its COM interface
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.Runtime.InteropServices;

[ComVisible(true), Guid("2B0C8F7E-6A1D-4C8E-9F3B-5D7A1E2C4B60"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwItem
{
    [PreserveSig]
    int Value();
}

[ComVisible(true), Guid("7E4A9D21-3B6F-4F0A-8C5E-1A2B3C4D5E6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ICcwItems
{
    [PreserveSig]
    int Next(uint count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] ICcwItem[] items, out uint fetched);

    [PreserveSig]
    int Sum(uint count, [In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] ICcwItem[] items);
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwItem : ICcwItem
{
    public int value;

    public int Value()
    {
        return value;
    }
}

[ComVisible(true), ClassInterface(ClassInterfaceType.None)]
public class CcwItems : ICcwItems
{
    public int Next(uint count, ICcwItem[] items, out uint fetched)
    {
        items[0] = new CcwItem { value = 7 };
        fetched = 1;
        return 0;
    }

    public int Sum(uint count, ICcwItem[] items)
    {
        int sum = 0;

        foreach (ICcwItem item in items)
        {
            if (item != null)
                sum += item.Value();
        }
        return sum;
    }
}

public static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int NextFn(IntPtr self, uint count, IntPtr items, out uint fetched);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int SumFn(IntPtr self, uint count, IntPtr items);

    static int failures;

    static void Check(bool condition, string message)
    {
        Console.WriteLine((condition ? "ok: " : "FAILED: ") + message);
        if (!condition)
            failures++;
    }

    static T Slot<T>(IntPtr itf, int index) where T : class
    {
        IntPtr vtable = Marshal.ReadIntPtr(itf);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vtable, index * IntPtr.Size), typeof(T));
    }

    public static int Main()
    {
        CcwItems items = new CcwItems();
        IntPtr itf = Marshal.GetComInterfaceForObject(items, typeof(ICcwItems));
        IntPtr buffer = Marshal.AllocCoTaskMem(2 * IntPtr.Size);
        uint fetched = 0;

        Marshal.WriteIntPtr(buffer, 0, IntPtr.Zero);
        Marshal.WriteIntPtr(buffer, IntPtr.Size, IntPtr.Zero);
        int hr = Slot<NextFn>(itf, 3)(itf, 2, buffer, out fetched);
        IntPtr first = Marshal.ReadIntPtr(buffer, 0);
        IntPtr second = Marshal.ReadIntPtr(buffer, IntPtr.Size);
        Check(hr == 0, "Next hr " + hr);
        Check(fetched == 1, "Next fetched " + fetched);
        Check(first != IntPtr.Zero, "Next returns the element as an interface pointer");
        Check(second == IntPtr.Zero, "Next returns NULL for the unset element");
        if (first != IntPtr.Zero)
        {
            ICcwItem item = Marshal.GetObjectForIUnknown(first) as ICcwItem;
            Check(item != null && item.Value() == 7, "Next element value");
            Check(Marshal.Release(first) == 0, "Next element is returned with one reference");
        }

        IntPtr element = Marshal.GetComInterfaceForObject(new CcwItem { value = 5 }, typeof(ICcwItem));
        Marshal.WriteIntPtr(buffer, 0, element);
        Marshal.WriteIntPtr(buffer, IntPtr.Size, IntPtr.Zero);
        int sum = Slot<SumFn>(itf, 4)(itf, 2, buffer);
        Check(sum == 5, "Sum of the elements passed in " + sum);
        Check(Marshal.ReadIntPtr(buffer, 0) == element, "Sum leaves the caller's array unchanged");
        Marshal.Release(element);

        Marshal.FreeCoTaskMem(buffer);
        Marshal.Release(itf);
        Console.WriteLine("ccwarray: " + failures + " failures");
        return failures;
    }
}
