// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using Microsoft.SymbolStore;
using System;
using System.Collections.Immutable;
using System.IO;

namespace Microsoft.Diagnostics.DebugServices
{
    public interface ISymbolService
    {
        /// <summary>
        /// Symbol file reader instance
        /// </summary>
        public class SymbolFile : IDisposable
        {
            public virtual void Dispose() 
            { 
            }
        }

        /// <summary>
        /// Invoked when anything changes in the symbol service (adding servers, caches, or directories, clearing store, etc.)
        /// </summary>
        IServiceEvent OnChangeEvent { get; }

        /// <summary>
        /// Returns true if symbol download has been enabled.
        /// </summary>
        bool IsSymbolStoreEnabled { get; }

        /// <summary>
        /// The default symbol cache path:
        /// 
        /// * dbgeng on Windows uses the dbgeng symbol cache path: %PROGRAMDATA%\dbg\sym
        /// * dotnet-dump on Windows uses the VS symbol cache path: %TEMPDIR%\SymbolCache
        /// * dotnet-dump/lldb on Linux/MacOS uses: $HOME/.dotnet/symbolcache
        /// </summary>
        string DefaultSymbolCache { get; set; }

        /// <summary>
        /// Parses the Windows debugger symbol path (srv*, cache*, etc.).
        /// </summary>
        /// <param name="symbolPath">Windows symbol path</param>
        /// <returns>if false, error parsing symbol path</returns>
        bool ParseSymbolPath(string symbolPath);

        /// <summary>
        /// Add symbol server to search path.
        /// </summary>
        /// <param name="msdl">if true, use the public Microsoft server</param>
        /// <param name="symweb">if true, use symweb internal server and protocol (file.ptr)</param>
        /// <param name="symbolServerPath">symbol server url (optional)</param>
        /// <param name="authToken"></param>
        /// <param name="timeoutInMinutes">symbol server timeout in minutes (optional)</param>
        /// <returns>if false, failure</returns>
        bool AddSymbolServer(bool msdl, bool symweb, string symbolServerPath, string authToken, int timeoutInMinutes);

        /// <summary>
        /// Add cache path to symbol search path
        /// </summary>
        /// <param name="symbolCachePath">symbol cache directory path (optional)</param>
        void AddCachePath(string symbolCachePath);

        /// <summary>
        /// Add directory path to symbol search path
        /// </summary>
        /// <param name="symbolDirectoryPath">symbol directory path to search (optional)</param>
        void AddDirectoryPath(string symbolDirectoryPath);

        /// <summary>
        /// This function disables any symbol downloading support.
        /// </summary>
        void DisableSymbolStore();

        /// <summary>
        /// Downloads module file
        /// </summary>
        /// <param name="module">module interface</param>
        /// <returns>module path or null</returns>
        string DownloadModule(IModule module);
        
        /// <summary>
        /// Download a file from the symbol stores/server.
        /// </summary>
        /// <param name="key">index of the file to download</param>
        /// <returns>path to the downloaded file either in the cache or in the temp directory or null if error</returns>
        string DownloadFile(SymbolStoreKey key);

        /// <summary>
        /// Returns the metadata for the assembly
        /// </summary>
        /// <param name="imagePath">file name and path to module</param>
        /// <param name="imageTimestamp">module timestamp</param>
        /// <param name="imageSize">size of PE image</param>
        /// <returns>metadata</returns>
        ImmutableArray<byte> GetMetadata(string imagePath, uint imageTimestamp, uint imageSize);

        /// <summary>
        /// Returns the portable PDB reader for the assembly path
        /// </summary>
        /// <param name="assemblyPath">file path of the assembly or null if the module is in-memory or dynamic</param>
        /// <param name="isFileLayout">type of in-memory PE layout, if true, file based layout otherwise, loaded layout</param>
        /// <param name="peStream">in-memory PE stream</param>
        /// <returns>symbol file or null</returns>
        /// <remarks>
        /// Assumes that neither PE image nor PDB loaded into memory can be unloaded or moved around.
        /// </remarks>
        ISymbolFile OpenSymbolFile(string assemblyPath, bool isFileLayout, Stream peStream);

        /// <summary>
        /// Returns the portable PDB reader for the portable PDB stream
        /// </summary>
        /// <param name="pdbStream">portable PDB memory or file stream</param>
        /// <returns>symbol file or null</returns>
        /// <remarks>
        /// Assumes that the PDB loaded into memory can be unloaded or moved around.
        /// </remarks>
        ISymbolFile OpenSymbolFile(Stream pdbStream);
    }
}
