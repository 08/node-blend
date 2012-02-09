#include "blend.hpp"
#include "writer.hpp"

#include <sstream>
#include <memory>
#include <cstring>

using namespace v8;
using namespace node;


unsigned int hexToUInt32Color(char *hex) {
    if (!hex) return 0;
    if (hex[0] == '#') hex++;
    int len = strlen(hex);
    if (len != 6 && len != 8) return 0;

    unsigned int color = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> color;

    if (len == 8) {
        // Circular shift to get from RGBA to ARGB.
        return (color << 24) | ((color & 0xFF00) << 8) | ((color & 0xFF0000) >> 8) | ((color & 0xFF000000) >> 24);
    } else {
        return 0xFF000000 | ((color & 0xFF) << 16) | (color & 0xFF00) | ((color & 0xFF0000) >> 16);
    }
}


Handle<Value> Blend(const Arguments& args) {
    HandleScope scope;
    std::auto_ptr<BlendBaton> baton(new BlendBaton());

    Local<Object> options;
    if (args.Length() == 0 || !args[0]->IsArray()) {
        return TYPE_EXCEPTION("First argument must be an array of Buffers.");
    } else if (args.Length() == 1) {
        return TYPE_EXCEPTION("Second argument must be a function");
    } else if (args.Length() == 2) {
        // No options provided.
        if (!args[1]->IsFunction()) {
            return TYPE_EXCEPTION("Second argument must be a function.");
        }
        baton->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
    } else if (args.Length() >= 3) {
        if (!args[1]->IsObject()) {
            return TYPE_EXCEPTION("Second argument must be a an options object.");
        }
        options = Local<Object>::Cast(args[1]);

        if (!args[2]->IsFunction()) {
            return TYPE_EXCEPTION("Third argument must be a function.");
        }
        baton->callback = Persistent<Function>::New(Local<Function>::Cast(args[2]));
    }

    // Validate options
    if (!options.IsEmpty()) {
        baton->quality = options->Get(String::NewSymbol("quality"))->Int32Value();

        Local<Value> format_val = options->Get(String::NewSymbol("format"));
        if (!format_val.IsEmpty() && format_val->BooleanValue()) {
            if (strcmp(*String::AsciiValue(format_val), "jpeg") == 0 ||
                    strcmp(*String::AsciiValue(format_val), "jpg") == 0) {
                baton->format = BLEND_FORMAT_JPEG;
                if (baton->quality == 0) baton->quality = 80;
                else if (baton->quality < 0 || baton->quality > 100) {
                    return TYPE_EXCEPTION("JPEG quality is range 0-100.");
                }
            } else if (strcmp(*String::AsciiValue(format_val), "png") == 0) {
                if (baton->quality == 1 || baton->quality > 256) {
                    return TYPE_EXCEPTION("PNG images must be quantized between 2 and 256 colors.");
                }
            } else {
                return TYPE_EXCEPTION("Invalid output format.");
            }
        }

        baton->reencode = options->Get(String::NewSymbol("reencode"))->BooleanValue();
        baton->width = options->Get(String::NewSymbol("width"))->Int32Value();
        baton->height = options->Get(String::NewSymbol("height"))->Int32Value();

        Local<Value> matte_val = options->Get(String::NewSymbol("matte"));
        if (!matte_val.IsEmpty() && matte_val->IsString()) {
            baton->matte = hexToUInt32Color(*String::AsciiValue(matte_val->ToString()));

            // Make sure we're reencoding in the case of single alpha PNGs
            if (baton->matte && !baton->reencode) {
                baton->reencode = true;
            }
        }
    }

    Local<Array> images = Local<Array>::Cast(args[0]);
    uint32_t length = images->Length();
    if (length < 1 && !baton->reencode) {
        return TYPE_EXCEPTION("First argument must contain at least one Buffer.");
    } else if (length == 1 && !baton->reencode) {
        Local<Value> buffer = images->Get(0);
        if (Buffer::HasInstance(buffer)) {
            // Directly pass through buffer if it's the only one.
            Local<Value> argv[] = {
                Local<Value>::New(Null()),
                buffer
            };
            TRY_CATCH_CALL(Context::GetCurrent()->Global(), baton->callback, 2, argv);
            return scope.Close(Undefined());
        } else {
            // Check whether the argument is a complex image with offsets etc.
            // In that case, we don't throw but continue going through the blend
            // process below.
            bool valid = false;
            if (buffer->IsObject()) {
                Local<Object> props = buffer->ToObject();
                valid = props->Has(String::NewSymbol("buffer")) &&
                        Buffer::HasInstance(props->Get(String::NewSymbol("buffer")));
            }
            if (!valid) {
                return TYPE_EXCEPTION("All elements must be Buffers or objects with a 'buffer' property.");
            }
        }
    }

    for (uint32_t i = 0; i < length; i++) {
        ImagePtr image(new Image());
        Local<Value> buffer = images->Get(i);
        if (Buffer::HasInstance(buffer)) {
            image->buffer = Persistent<Object>::New(buffer->ToObject());
        } else if (buffer->IsObject()) {
            Local<Object> props = buffer->ToObject();
            if (props->Has(String::NewSymbol("buffer"))) {
                Local<Value> buffer = props->Get(String::NewSymbol("buffer"));
                if (Buffer::HasInstance(buffer)) {
                    image->buffer = Persistent<Object>::New(buffer->ToObject());
                }
            }
            image->x = props->Get(String::NewSymbol("x"))->Int32Value();
            image->y = props->Get(String::NewSymbol("y"))->Int32Value();
        }

        if (image->buffer.IsEmpty()) {
            return TYPE_EXCEPTION("All elements must be Buffers or objects with a 'buffer' property.");
        }

        image->data = (unsigned char*)node::Buffer::Data(image->buffer);
        image->dataLength = node::Buffer::Length(image->buffer);
        baton->images.push_back(image);
    }

    uv_queue_work(uv_default_loop(), &baton.release()->request, Work_Blend, Work_AfterBlend);

    return scope.Close(Undefined());
}

// inline void Blend_CompositeTopDown(unsigned int* images[], int size, unsigned long width, unsigned long height) {
//     size_t length = width * height;
//     unsigned int *target = images[0];
//     for (long px = length - 1; px >= 0; px--) {
//         // Starting pixel
//         unsigned int abgr = target[px];

//         // Skip if topmost pixel is opaque.
//         if (abgr >= 0xFF000000) continue;

//         for (int i = 1; i < size; i++) {
//             unsigned int *source = images[i];
//             if (source[px] <= 0x00FFFFFF) {
//                 // Lower pixel is fully transparent.
//                 continue;
//             } else if (abgr <= 0x00FFFFFF) {
//                 // Upper pixel is fully transparent.
//                 abgr = source[px];
//             } else {
//                 // Both pixels have transparency.
//                 unsigned int rgba0 = source[px];
//                 unsigned int rgba1 = abgr;

//                 // From http://trac.mapnik.org/browser/trunk/include/mapnik/graphics.hpp#L337
//                 unsigned a1 = (rgba1 >> 24) & 0xff;
//                 unsigned r1 = rgba1 & 0xff;
//                 unsigned g1 = (rgba1 >> 8) & 0xff;
//                 unsigned b1 = (rgba1 >> 16) & 0xff;

//                 unsigned a0 = (rgba0 >> 24) & 0xff;
//                 unsigned r0 = (rgba0 & 0xff) * a0;
//                 unsigned g0 = ((rgba0 >> 8) & 0xff) * a0;
//                 unsigned b0 = ((rgba0 >> 16) & 0xff) * a0;

//                 a0 = ((a1 + a0) << 8) - a0 * a1;

//                 r0 = ((((r1 << 8) - r0) * a1 + (r0 << 8)) / a0);
//                 g0 = ((((g1 << 8) - g0) * a1 + (g0 << 8)) / a0);
//                 b0 = ((((b1 << 8) - b0) * a1 + (b0 << 8)) / a0);
//                 a0 = a0 >> 8;
//                 abgr = (a0 << 24) | (b0 << 16) | (g0 << 8) | (r0);
//             }
//             if (abgr >= 0xFF000000) break;
//         }

//         // Merge pixel back.
//         target[px] = abgr;
//     }
// }

inline void Blend_CompositePixel(unsigned int& target, unsigned int& source) {
    if (source <= 0x00FFFFFF) {
        // Top pixel is fully transparent.
        // <do nothing>
    } else if (source >= 0xFF000000 || target <= 0x00FFFFFF) {
        // Top pixel is fully opaque or bottom pixel is fully transparent.
        target = source;
    } else {
        // Both pixels have transparency.
        // From http://trac.mapnik.org/browser/trunk/include/mapnik/graphics.hpp#L337
        unsigned int a1 = (source >> 24) & 0xff;
        unsigned int r1 = source & 0xff;
        unsigned int g1 = (source >> 8) & 0xff;
        unsigned int b1 = (source >> 16) & 0xff;

        unsigned int a0 = (target >> 24) & 0xff;
        unsigned int r0 = (target & 0xff) * a0;
        unsigned int g0 = ((target >> 8) & 0xff) * a0;
        unsigned int b0 = ((target >> 16) & 0xff) * a0;

        a0 = ((a1 + a0) << 8) - a0 * a1;
        r0 = ((((r1 << 8) - r0) * a1 + (r0 << 8)) / a0);
        g0 = ((((g1 << 8) - g0) * a1 + (g0 << 8)) / a0);
        b0 = ((((b1 << 8) - b0) * a1 + (b0 << 8)) / a0);
        a0 = a0 >> 8;
        target = (a0 << 24) | (b0 << 16) | (g0 << 8) | (r0);
    }
}


void Blend_Composite(unsigned int *target, BlendBaton *baton, Image *image) {
    unsigned int *source = image->reader->surface;

    int sourceX = std::max(0, -image->x);
    int sourceY = std::max(0, -image->y);
    int sourcePos = sourceY * image->width + sourceX;

    int width = image->width - sourceX - std::max(0, image->x + image->width - baton->width);
    int height = image->height - sourceY - std::max(0, image->y + image->height - baton->height);

    int targetX = std::max(0, image->x);
    int targetY = std::max(0, image->y);
    int targetPos = targetY * baton->width + targetX;

    // fprintf(stderr, "sourcePos: %d, targetPos: %d, width: %d, height: %d\n", sourcePos, targetPos, width, height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            Blend_CompositePixel(target[targetPos + x], source[sourcePos + x]);
        }

        sourcePos += image->width;
        targetPos += baton->width;
    }
}

void Work_Blend(uv_work_t* req) {
    BlendBaton* baton = static_cast<BlendBaton*>(req->data);

    int total = baton->images.size();
    bool alpha = true;
    int size = 0;

    // Iterate from the last to first image because we potentially don't have
    // to decode all images if there's an opaque one.
    Images::reverse_iterator rit = baton->images.rbegin();
    Images::reverse_iterator rend = baton->images.rend();
    for (int index = total - 1; rit != rend; rit++, index--) {
        // If an image that is higher than the current is opaque, stop alltogether.
        if (!alpha) break;

        Image *image = &**rit;
        std::auto_ptr<ImageReader> layer(ImageReader::create(image->data, image->dataLength));

        // Error out on invalid images.
        if (layer.get() == NULL || layer->width == 0 || layer->height == 0) {
            baton->message = layer->message;
            return;
        }

        int visibleWidth = (int)layer->width + image->x;
        int visibleHeight = (int)layer->height + image->y;

        // The first image that is in the viewport sets the width/height, if not user supplied.
        if (baton->width <= 0) baton->width = visibleWidth;
        if (baton->height <= 0) baton->height = visibleHeight;

        // Skip images that are outside of the viewport.
        if (visibleWidth <= 0 || visibleHeight <= 0 || image->x >= baton->width || image->y >= baton->height) {
            // Remove this layer from the list of layers we consider blending.
            continue;
        }

        // Short-circuit when we're not reencoding.
        if (size == 0 && !layer->alpha && !baton->reencode &&
            image->x == 0 && image->y == 0 &&
            (int)layer->width == baton->width && (int)layer->height == baton->height)
        {
            baton->result = (unsigned char *)malloc(image->dataLength);
            assert(baton->result);
            memcpy(baton->result, image->data, image->dataLength);
            baton->resultLength = image->dataLength;
            return;
        }

        if (!layer->decode()) {
            // Decoding failed.
            baton->message = layer->message;
            return;
        }
        else if (layer->warnings.size()) {
            std::vector<std::string>::iterator pos = layer->warnings.begin();
            std::vector<std::string>::iterator end = layer->warnings.end();
            for (; pos != end; pos++) {
                std::ostringstream msg;
                msg << "Layer " << index << ": " << *pos;
                baton->warnings.push_back(msg.str());
            }
        }

        bool coversWidth = image->x <= 0 && visibleWidth >= baton->width;
        bool coversHeight = image->y <= 0 && visibleHeight >= baton->height;
        if (!layer->alpha && coversWidth && coversHeight) {
            // Skip decoding more layers.
            alpha = false;
        }

        // Convenience aliases.
        image->width = layer->width;
        image->height = layer->height;
        image->reader = layer;
        size++;
    }

    // Now blend images.
    int pixels = baton->width * baton->height;
    if (pixels == 0) {
        std::ostringstream msg;
        msg << "Image dimensions " << baton->width << "x" << baton->height << " are invalid";
        baton->message = msg.str();
        return;
    }

    unsigned int *target = (unsigned int *)malloc(sizeof(unsigned int) * pixels);
    if (!target) {
        baton->message = "Memory allocation failed";
        return;
    }

    // When we don't actually have transparent pixels, we don't need to set
    // the matte.
    if (alpha) {
        for (int i = 0; i < pixels; i++) {
            target[i] = baton->matte;
        }
    }

    for (Images::iterator it = baton->images.begin(); it != baton->images.end(); it++) {
        Image *image = &**it;
        if (image->reader.get()) {
            Blend_Composite(target, baton, image);
        }
    }

    Blend_Encode((unsigned char*)target, baton, alpha);
    free(target);
    target = NULL;
}

void freeBuffer(char *data, void *hint) {
    free(data);
    data = NULL;
}

void Work_AfterBlend(uv_work_t* req) {
    HandleScope scope;
    BlendBaton* baton = static_cast<BlendBaton*>(req->data);

    if (!baton->message.length() && baton->result) {
        Local<Array> warnings = Array::New();
        std::vector<std::string>::iterator pos = baton->warnings.begin();
        std::vector<std::string>::iterator end = baton->warnings.end();
        for (int i = 0; pos != end; pos++, i++) {
            warnings->Set(i, String::New((*pos).c_str()));
        }

        // In the success case, node's Buffer implementation frees the result pointer for us.
        Local<Value> argv[] = {
            Local<Value>::New(Null()),
            Local<Value>::New(Buffer::New((char*)baton->result, baton->resultLength, freeBuffer, NULL)->handle_),
            Local<Value>::New(warnings)
        };
        TRY_CATCH_CALL(Context::GetCurrent()->Global(), baton->callback, 3, argv);
    } else {
        Local<Value> argv[] = {
            Local<Value>::New(Exception::Error(String::New(baton->message.c_str())))
        };

        // In the error case, we have to manually free this.
        if (baton->result) {
            free(baton->result);
            baton->result = NULL;
        }

        assert(!baton->callback.IsEmpty());
        TRY_CATCH_CALL(Context::GetCurrent()->Global(), baton->callback, 1, argv);
    }

    delete baton;
}

extern "C" void init(Handle<Object> target) {
    NODE_SET_METHOD(target, "blend", Blend);

    target->Set(
        String::NewSymbol("libpng"),
        String::NewSymbol(PNG_LIBPNG_VER_STRING),
        static_cast<PropertyAttribute>(ReadOnly | DontDelete)
    );

    target->Set(
        String::NewSymbol("libjpeg"),
        Integer::New(JPEG_LIB_VERSION),
        static_cast<PropertyAttribute>(ReadOnly | DontDelete)
    );
}
