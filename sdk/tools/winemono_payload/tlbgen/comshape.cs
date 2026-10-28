/*
 * PROJECT:     LiberNT Wine Mono payload
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Applies the COM visibility and class interface attributes of the .NET Framework mscorlib
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

using System;
using System.IO;
using System.Linq;
using Mono.Cecil;

static class ComShape
{
    static TypeDefinition Find(ModuleDefinition module, string name)
    {
        var type = module.GetType(name);
        if (type == null)
            throw new Exception("type not found: " + name);
        return type;
    }

    static MethodReference Ctor(ModuleDefinition module, string attribute, string parameter)
    {
        var type = Find(module, attribute);
        return module.ImportReference(type.Methods.Single(m => m.IsConstructor && !m.IsStatic && m.Parameters.Count == 1 && m.Parameters[0].ParameterType.FullName == parameter));
    }

    static void Replace(TypeDefinition type, MethodReference ctor, CustomAttributeArgument argument)
    {
        foreach (var old in type.CustomAttributes.Where(a => a.AttributeType.FullName == ctor.DeclaringType.FullName).ToList())
            type.CustomAttributes.Remove(old);
        var attribute = new CustomAttribute(ctor);
        attribute.ConstructorArguments.Add(argument);
        type.CustomAttributes.Add(attribute);
    }

    static int Main(string[] args)
    {
        var module = ModuleDefinition.ReadModule(args[0], new ReaderParameters { ReadWrite = false, InMemory = true });
        var visible = Ctor(module, "System.Runtime.InteropServices.ComVisibleAttribute", "System.Boolean");
        var classItf = Ctor(module, "System.Runtime.InteropServices.ClassInterfaceAttribute", "System.Runtime.InteropServices.ClassInterfaceType");
        var defaultItf = Ctor(module, "System.Runtime.InteropServices.ComDefaultInterfaceAttribute", "System.Type");
        var classItfType = Find(module, "System.Runtime.InteropServices.ClassInterfaceType");
        foreach (var line in File.ReadAllLines(args[1]))
        {
            var f = line.Split(' ');
            if (f.Length == 0 || f[0].Length == 0 || f[0].StartsWith("#"))
                continue;
            var type = Find(module, f[1]);
            switch (f[0])
            {
            case "ComVisible":
                Replace(type, visible, new CustomAttributeArgument(module.TypeSystem.Boolean, true));
                break;
            case "ClassInterface":
                Replace(type, classItf, new CustomAttributeArgument(classItfType, int.Parse(f[2])));
                break;
            case "MemberOrder":
                break;
            case "ComDefaultInterface":
                Replace(type, defaultItf, new CustomAttributeArgument(module.ImportReference(Find(module, "System.Type")), Find(module, f[2])));
                break;
            default:
                throw new Exception("unknown shape: " + line);
            }
        }
        module.Write(args[2]);
        return 0;
    }
}
