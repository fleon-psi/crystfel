module Images

import ..CrystFEL: libcrystfel
import ..CrystFEL.DataTemplates: DataTemplate, InternalDataTemplate
import ..CrystFEL.DetGeoms: DetGeom
import ..CrystFEL.PeakLists: PeakList, InternalPeakList
import ..CrystFEL.Crystals: Crystal, InternalCrystal
export Image

const HEADER_CACHE_SIZE = 128

mutable struct InternalImage
    dp::Ptr{Ptr{Cfloat}}
    bad::Ptr{Ptr{Cint}}
    sat::Ptr{Ptr{Cfloat}}
    hit::Cint
    crystals::Ptr{Ptr{Cvoid}}
    n_crystals::Cint
    indexed_by::Cint
    n_indexing_tries::Cint
    detgeom::Ptr{DetGeom}
    data_source_type::Cint
    filename::Cstring
    ev::Cstring
    data_block::Ptr{Cvoid}
    data_block_size::Csize_t
    meta_data::Cstring
    header_cache::NTuple{HEADER_CACHE_SIZE, Ptr{Cvoid}}
    n_cached_headers::Cint
    id::Cint
    serial::Cint
    spectrum::Ptr{Cvoid}
    lambda::Cdouble
    div::Cdouble
    bw::Cdouble
    peak_resolution::Cdouble
    peaklist::Ptr{InternalPeakList}
    ida::Ptr{Cvoid}
end


mutable struct Image
    internalptr::Ptr{InternalImage}
end


function makecrystallist(listptr, n)

    crystals = Crystal[]

    if listptr == C_NULL
        return crystals
    end

    for i in 1:n
        crystalptr = unsafe_load(listptr, i)
        push!(crystals, Crystal(crystalptr, true))
    end

    crystals

end


function Base.getproperty(image::Image, name::Symbol)
    if name === :internalptr
        getfield(image, :internalptr)
    else
        idata = unsafe_load(image.internalptr)

        if name === :peaklist
            let pl = getproperty(idata, :peaklist)
                if pl == C_NULL
                    throw(ErrorException("Image doesn't have a peak list"))
                else
                    PeakList(pl, true)
                end
            end

        elseif name === :crystals
            return makecrystallist(getproperty(idata, :crystals),
                                   getproperty(idata, :n_crystals))

        else
            getproperty(idata, name)
        end
    end
end


strdup(str) = @ccall strdup(str::Cstring)::Cstring


function assert_type(val, type)
    if !(val isa type)
        throw(ArgumentError("Must be a "*string(type)*" (have "*string(typeof(val))*" instead)"))
    end
end


function set_peaklist(image, val)
    assert_type(val, PeakList)
    if val.in_image
        throw(ArgumentError("PeakList is already in an image.  "*
                            "Add a copy (use `deepcopy`) instead."))
    end
    val.in_image = true
    val = val.internalptr
    idata = unsafe_load(image.internalptr)
    old = swapproperty!(idata, :peaklist, val)
    unsafe_store!(image.internalptr, idata)

    if old != C_NULL
        @ccall libcrystfel.image_feature_list_free(old::Ptr{InternalPeakList})::Cvoid
    end

end


function Base.setproperty!(image::Image, name::Symbol, val)
    if name === :internalptr
        setfield!(image, :internalptr, val)
    else

        if name === :peaklist
            return set_peaklist(image, val)

        elseif name === :filename
            assert_type(val, AbstractString)
            val = strdup(val)

        elseif name === :ev
            assert_type(val, AbstractString)
            val = strdup(val)

        end

        idata = unsafe_load(image.internalptr)
        setproperty!(idata, name, val)
        unsafe_store!(image.internalptr, idata)

    end
end


function Base.propertynames(image::Image; private=false)
    if private
        fieldnames(InternalImage)
    else
        tuple(fieldnames(InternalImage)..., :internalptr)
    end
end


function Base.push!(image::Image, cr::Crystal)
    if cr.in_image
        throw(ErrorException("Crystal is already in an image"))
    end
    ccall((:image_add_crystal, libcrystfel),
          Cvoid, (Ptr{InternalImage},Ptr{InternalCrystal}),
          image.internalptr, cr.internalptr)
    cr.in_image = true
end


"""
    Image(dtempl::DataTemplate)

Creates a CrystFEL image structure, not linked to any file or data block,
i.e. for simulation purposes.  This will fail if `dtempl` contains any
references to metadata fields, e.g. `photon_energy = /LCLS/photon_energy eV`.

Corresponds to CrystFEL C API function `image_create_for_simulation()`.
"""
function Image(dtempl::DataTemplate)

    out = ccall((:image_create_for_simulation, libcrystfel),
                Ptr{Image}, (Ref{InternalDataTemplate},), dtempl.internalptr)
    if out == C_NULL
        throw(ArgumentError("Failed to create image"))
    end

    image = Image(out)

    finalizer(image) do x
        ccall((:image_free, libcrystfel), Cvoid, (Ptr{InternalImage},), x.internalptr)
    end

    return image
end


"""
    Image(dtempl::DataTemplate, filename::AbstractString, event::AbstractString,
          no_image_data=false, no_mask_data=false)

Loads an image from the filesystem.

Corresponds to CrystFEL C API function `image_read()`.
"""
function Image(dtempl::DataTemplate,
               filename::AbstractString,
               event::AbstractString="//",
               no_image_data=false,
               no_mask_data=false)

    out = @ccall libcrystfel.image_read(dtempl.internalptr::Ptr{InternalDataTemplate},
                                        filename::Cstring, event::Cstring,
                                        no_image_data::Cint, no_mask_data::Cint,
                                        C_NULL::Ptr{Cvoid})::Ptr{Image}
    if out == C_NULL
        throw(ArgumentError("Failed to load image"))
    end

    image = Image(out)

    finalizer(image) do x
        ccall((:image_free, libcrystfel), Cvoid, (Ptr{InternalImage},), x.internalptr)
    end

    return image
end


end  # of module
